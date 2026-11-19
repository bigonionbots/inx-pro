/**
 * @file UsbAudioSink.cpp
 * @brief Definitions for UsbAudioSink.
 */

#include "UsbAudioSink.h"

#if INX_HAS_AUDIO_MODULE

#include <Arduino.h>
#include <SDCardManager.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>

#include "AudioModule.h"
#include "UsbAudioOut.h"
#include "audio/PcmDecoder.h"
#include "audio/Resampler.h"
#include "util/SdIoMutex.h"

namespace UsbAudioSink {

namespace {

using audiolink::Codec;
using audiolink::PlayState;
using audiolink::Type;

/** Compressed audio waiting for the decoder: about 4 s at 128 kbit/s (the player queues a minute). */
constexpr size_t RING_BYTES = 64 * 1024;
/** The decoder's view of the stream: room for the largest ADTS frame and minimp3's resync. */
constexpr size_t WINDOW_BYTES = 16 * 1024;
constexpr uint32_t WRITE_TIMEOUT_MS = 100;
constexpr uint8_t DEFAULT_VOLUME = 40;
constexpr const char* VOLUME_FILE = "/.system/headphones.txt";
constexpr uint32_t VOLUME_SAVE_DELAY_MS = 3000;
/** A playing stream logs its decode load this often. */
constexpr uint32_t LOAD_LOG_MS = 60000;

constexpr UBaseType_t TASK_PRIORITY = 4;  // under the USB host and UAC driver tasks (5)
/**
 * The application core, away from Wi-Fi (core 0), above the main loop and the stream feeder there,
 * so a TLS handshake or a page layout cannot starve the adapter's small buffer.
 */
constexpr BaseType_t TASK_CORE = 1;
constexpr uint32_t TASK_STACK_BYTES = 20 * 1024;

void* psramAlloc(const size_t bytes) { return heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }
void psramFree(void* block) { heap_caps_free(block); }

SemaphoreHandle_t lock = nullptr;  // guards everything below
uint8_t* ring = nullptr;
size_t ringHead = 0;
size_t ringTail = 0;
size_t ringUsed = 0;
/** Bumped by STREAM_START and STREAM_STOP: the decode task drops work from an older one. */
uint32_t generation = 0;
uint8_t streamId = 0;
Codec codec = Codec::Mp3;
PlayState state = PlayState::Idle;
bool paused = false;
bool ended = false;
uint8_t channels = 0;
uint32_t sampleRate = 0;
uint64_t playedUs = 0;
uint16_t underruns = 0;
uint16_t decodeErrors = 0;
size_t windowBytes = 0;  // for STATUS rxBuffered

std::atomic<uint8_t> volume{DEFAULT_VOLUME};
std::atomic<bool> volumeToApply{true};
std::atomic<bool> volumeUnsaved{false};
uint32_t volumeChangedMs = 0;

class LockGuard {
 public:
  LockGuard() { xSemaphoreTake(lock, portMAX_DELAY); }
  ~LockGuard() { xSemaphoreGive(lock); }
  LockGuard(const LockGuard&) = delete;
  LockGuard& operator=(const LockGuard&) = delete;
};

void clearStreamLocked() {
  ringHead = ringTail = ringUsed = 0;
  ++generation;
  ended = false;
  channels = 0;
  sampleRate = 0;
  playedUs = 0;
  underruns = 0;
  decodeErrors = 0;
}

/** Up to `room` ring bytes into out (lock held). */
size_t takeLocked(uint8_t* out, const size_t room) {
  size_t taken = 0;
  while (taken < room && ringUsed > 0) {
    const size_t run = std::min({room - taken, ringUsed, RING_BYTES - ringTail});
    std::memcpy(out + taken, ring + ringTail, run);
    ringTail = (ringTail + run) % RING_BYTES;
    ringUsed -= run;
    taken += run;
  }
  return taken;
}

PcmDecoder::Codec decoderCodec(const Codec c) {
  switch (c) {
    case Codec::Mp3:
      return PcmDecoder::Codec::Mp3;
    case Codec::AacAdts:
      return PcmDecoder::Codec::AacAdts;
    default:
      return PcmDecoder::Codec::None;
  }
}

const char* codecName(const Codec c) {
  switch (c) {
    case Codec::Mp3:
      return "MP3";
    case Codec::AacAdts:
      return "AAC";
    default:
      return "unsupported codec";
  }
}

/**
 * The decode task: stream -> PcmDecoder -> Resampler -> UsbAudioOut. Its buffers are in PSRAM
 * and it never touches flash or the SD card, so its stack can be in PSRAM too.
 */
void decodeTaskMain(void*) {
  auto* window = static_cast<uint8_t*>(psramAlloc(WINDOW_BYTES));
  auto* pcm = static_cast<int16_t*>(psramAlloc(PcmDecoder::MAX_FRAMES * 2 * sizeof(int16_t)));
  // Up to 2048 frames at 8 kHz to 48 kHz: 6x, plus the filter's lag.
  constexpr size_t OUT_FRAMES = PcmDecoder::MAX_FRAMES * 6 + 64;
  auto* out = static_cast<int16_t*>(psramAlloc(OUT_FRAMES * 2 * sizeof(int16_t)));
  if (!window || !pcm || !out) {
    AudioModule::logf("Headphones: out of memory for decoding");
    vTaskSuspend(nullptr);
  }
  PcmDecoder decoder;
  Resampler resampler;
  size_t windowLen = 0;
  uint32_t windowGeneration = UINT32_MAX;
  bool decoderReady = false;
  bool starving = false;
  bool ownsStream = false;  // this task started the adapter's stream (not the test tone)
  uint32_t loadLogMs = 0;
  uint64_t decodeUs = 0;
  uint64_t audioUs = 0;

  for (;;) {
    // Kept by UsbAudioOut while no stream runs, and applied when one starts.
    if (volumeToApply.exchange(false)) UsbAudioOut::setVolume(volume.load());
    if (!UsbAudioOut::isConnected()) {
      ownsStream = false;
      vTaskDelay(pdMS_TO_TICKS(50));
      continue;
    }

    uint32_t gen;
    PlayState now;
    bool isPaused, isEnded;
    Codec streamCodec;
    {
      LockGuard guard;
      gen = generation;
      now = state;
      isPaused = paused;
      isEnded = ended;
      streamCodec = codec;
    }
    if (gen != windowGeneration) {
      windowGeneration = gen;
      windowLen = 0;
      starving = false;
      decoderReady = decoder.begin(decoderCodec(streamCodec), psramAlloc, psramFree);
      if (!decoderReady && now != PlayState::Idle) {
        AudioModule::logf("Headphones: cannot decode %s", codecName(streamCodec));
      }
      resampler.reset();
    }
    if (now == PlayState::Idle && ownsStream) {
      UsbAudioOut::streamStop();  // the book stopped: free the adapter (the test tone needs it)
      ownsStream = false;
    }
    if (now == PlayState::Idle || now == PlayState::Finished || isPaused || !decoderReady) {
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }

    bool ringEmpty;
    {
      LockGuard guard;
      if (generation != gen) continue;
      windowLen += takeLocked(window + windowLen, WINDOW_BYTES - windowLen);
      ringEmpty = ringUsed == 0;
      windowBytes = windowLen;
    }

    const uint32_t startUs = micros();
    const PcmDecoder::Frame frame = decoder.decode(window, windowLen, pcm, isEnded && ringEmpty);
    decodeUs += micros() - startUs;
    size_t consumed = frame.consumed;
    if (consumed == 0 && windowLen == WINDOW_BYTES) {
      consumed = WINDOW_BYTES / 2;  // a full window and still no frame: not this codec, skip on
      LockGuard guard;
      if (generation == gen) ++decodeErrors;
    }
    if (consumed > 0) {
      std::memmove(window, window + consumed, windowLen - consumed);
      windowLen -= consumed;
    }
    if (frame.error) {
      LockGuard guard;
      if (generation == gen) ++decodeErrors;
    }

    if (frame.frames > 0) {
      starving = false;
      const uint32_t outRate = UsbAudioOut::bestRate(frame.sampleRate);
      if (outRate == 0) {
        vTaskDelay(pdMS_TO_TICKS(50));
        continue;
      }
      if (UsbAudioOut::streamRate() != outRate) {
        if (!UsbAudioOut::streamStart(outRate)) {
          vTaskDelay(pdMS_TO_TICKS(200));
          continue;
        }
        ownsStream = true;
        AudioModule::logf("Headphones: %s %lu Hz %u ch -> adapter %lu Hz", codecName(streamCodec),
                          static_cast<unsigned long>(frame.sampleRate), frame.channels,
                          static_cast<unsigned long>(outRate));
      }
      if (resampler.inRate() != frame.sampleRate || resampler.outRate() != outRate) {
        resampler.configure(frame.sampleRate, outRate, psramAlloc, psramFree);
      }
      const uint32_t resampleUs = micros();
      const size_t n = resampler.process(pcm, frame.frames, out);
      decodeUs += micros() - resampleUs;
      const uint64_t frameUs = static_cast<uint64_t>(frame.frames) * 1000000ull / frame.sampleRate;
      {
        LockGuard guard;
        if (generation != gen) continue;
        channels = frame.channels;
        sampleRate = frame.sampleRate;
        if (state == PlayState::Buffering) state = PlayState::Playing;
      }
      size_t written = 0;
      while (written < n) {
        written += UsbAudioOut::streamWrite(out + 2 * written, n - written, WRITE_TIMEOUT_MS);
        LockGuard guard;
        if (generation != gen || paused || !UsbAudioOut::isConnected()) break;
      }
      {
        LockGuard guard;
        if (generation == gen) playedUs += frameUs;
      }
      audioUs += frameUs;
      if (millis() - loadLogMs >= LOAD_LOG_MS && audioUs > 0) {
        if (loadLogMs != 0) {
          AudioModule::logf("Headphones: decoding takes %lu%% of real time (%s %lu Hz)",
                            static_cast<unsigned long>(decodeUs * 100 / audioUs), codecName(streamCodec),
                            static_cast<unsigned long>(frame.sampleRate));
        }
        loadLogMs = millis();
        decodeUs = 0;
        audioUs = 0;
      }
      continue;
    }

    if (consumed > 0) continue;  // skipped junk or a damaged frame: try the next

    if (isEnded && ringEmpty) {
      // Everything decoded: let the adapter play out its buffer, then report the end.
      {
        LockGuard guard;
        if (generation != gen || state == PlayState::Finished) continue;
        state = PlayState::Draining;
      }
      vTaskDelay(pdMS_TO_TICKS(UsbAudioOut::streamBufferedMs() + 20));
      LockGuard guard;
      if (generation == gen) state = PlayState::Finished;
      continue;
    }
    if (ringEmpty && !starving) {
      starving = true;
      LockGuard guard;
      if (generation == gen && state == PlayState::Playing) ++underruns;
    }
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

}  // namespace

void begin() {
  if (lock) return;
  lock = xSemaphoreCreateMutex();
  ring = static_cast<uint8_t*>(psramAlloc(RING_BYTES));
  if (!lock || !ring) {
    AudioModule::logf("Headphones: out of memory");
    return;
  }
  {
    SdIoMutex::Lock sd;
    FsFile file = SdMan.open(VOLUME_FILE, O_RDONLY);
    if (file) {
      char text[8] = {};
      file.read(text, sizeof(text) - 1);
      file.close();
      const int saved = atoi(text);
      if (saved >= 0 && saved <= 100 && text[0] != '\0') volume = static_cast<uint8_t>(saved);
    }
  }
  xTaskCreatePinnedToCoreWithCaps(decodeTaskMain, "usb_audio_dec", TASK_STACK_BYTES, nullptr, TASK_PRIORITY, nullptr,
                                  TASK_CORE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

bool ready() { return lock && ring && UsbAudioOut::isConnected() && UsbAudioOut::device().rateCount > 0; }

uint32_t sessionCounter() { return UsbAudioOut::connectCounter(); }

bool send(const Type type, const void* payload, const size_t length, uint32_t) {
  if (!ready()) return false;
  const auto* bytes = static_cast<const uint8_t*>(payload);
  switch (type) {
    case Type::StreamStart: {
      if (length < sizeof(audiolink::StreamStartPayload)) return false;
      audiolink::StreamStartPayload start;
      std::memcpy(&start, payload, sizeof(start));
      LockGuard guard;
      clearStreamLocked();
      streamId = start.streamId;
      codec = static_cast<Codec>(start.codec);
      state = PlayState::Buffering;
      return true;
    }
    case Type::StreamEnd: {
      LockGuard guard;
      if (length >= 1 && bytes[0] == streamId) ended = true;
      return true;
    }
    case Type::StreamStop: {
      LockGuard guard;
      if (length >= 1 && bytes[0] == streamId) {
        clearStreamLocked();
        state = PlayState::Idle;
      }
      return true;
    }
    case Type::Pause: {
      LockGuard guard;
      paused = true;
      if (state == PlayState::Playing || state == PlayState::Buffering) state = PlayState::Paused;
      return true;
    }
    case Type::Resume: {
      LockGuard guard;
      paused = false;
      if (state == PlayState::Paused) state = PlayState::Buffering;
      return true;
    }
    case Type::SetVolume:
      if (length >= 1) {
        volume = std::min<uint8_t>(bytes[0], 100);
        volumeToApply = true;
        volumeUnsaved = true;
        volumeChangedMs = millis();
      }
      return true;
    default:
      // Ping, output selection and Bluetooth have nothing to do here.
      return true;
  }
}

bool sendAudio(const uint8_t id, const uint8_t* data, const size_t length, const uint32_t timeoutMs) {
  const uint32_t startMs = millis();
  for (;;) {
    if (!ready()) return false;
    {
      LockGuard guard;
      if (id != streamId || state == PlayState::Idle) return true;  // an old stream's data: drop it
      if (RING_BYTES - ringUsed >= length) {
        size_t done = 0;
        while (done < length) {
          const size_t run = std::min(length - done, RING_BYTES - ringHead);
          std::memcpy(ring + ringHead, data + done, run);
          ringHead = (ringHead + run) % RING_BYTES;
          ringUsed += run;
          done += run;
        }
        return true;
      }
    }
    if (millis() - startMs >= timeoutMs) return false;
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

audiolink::StatusPayload status() {
  audiolink::StatusPayload s{};
  s.output = static_cast<uint8_t>(USB_HEADPHONES);
  s.btState = static_cast<uint8_t>(audiolink::BtState::Off);
  s.volume = volume.load();
  s.freeHeap = static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  if (!lock) return s;
  LockGuard guard;
  s.streamId = streamId;
  s.playState = static_cast<uint8_t>(state);
  s.channels = channels;
  s.sampleRate = sampleRate;
  s.playedMs = static_cast<uint32_t>(playedUs / 1000);
  s.rxBuffered = static_cast<uint32_t>(ringUsed + windowBytes);
  s.pcmBufferedMs = state == PlayState::Playing ? static_cast<uint16_t>(UsbAudioOut::streamBufferedMs()) : 0;
  s.underruns = underruns;
  s.decodeErrors = decodeErrors;
  return s;
}

void service() {
  if (!volumeUnsaved || millis() - volumeChangedMs < VOLUME_SAVE_DELAY_MS) return;
  volumeUnsaved = false;
  char text[8];
  const int n = snprintf(text, sizeof(text), "%u\n", volume.load());
  SdIoMutex::Lock sd;
  SdMan.mkdir("/.system");
  FsFile file = SdMan.open(VOLUME_FILE, O_WRONLY | O_CREAT | O_TRUNC);
  if (!file) return;
  file.write(text, n);
  file.close();
}

}  // namespace UsbAudioSink

#endif  // INX_HAS_AUDIO_MODULE
