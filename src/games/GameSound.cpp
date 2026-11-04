/**
 * @file GameSound.cpp
 * @brief Definitions for game sound through the external audio module.
 */

#include "GameSound.h"

#if INX_HAS_GAMES

#include <Arduino.h>
#include <SDCardManager.h>

#include <cstdio>

#include "util/SdIoMutex.h"

#if INX_HAS_AUDIO_MODULE
#include <AudioLinkProtocol.h>
#include <MuLaw.h>
#include <Resample2x.h>
#include <freertos/FreeRTOS.h>
#include <freertos/stream_buffer.h>

#include <atomic>

#include "system/AudioLink.h"
#include "system/AudioModule.h"
#endif

namespace GameSound {

namespace {

constexpr const char* SETTING_FILE = "/Games/.sound";

#if INX_HAS_AUDIO_MODULE
/** µ-law bytes (= samples) waiting for the link: about 93 ms. More than that is dropped. */
constexpr size_t QUEUE_BYTES = 2048;
/** Bytes per AUDIO_DATA frame. */
constexpr size_t SEND_CHUNK = audiolink::AUDIO_CHUNK;
constexpr uint32_t SENDER_STACK = 4096;
constexpr int SENDER_PRIORITY = 3;

bool active_ = false;
StreamBufferHandle_t queue = nullptr;
std::atomic<bool> streaming{false};  // a stream is open on the module
std::atomic<uint8_t> streamId{0};    // its id
std::atomic<uint32_t> dropped{0};    // times audio was dropped because the queue was full
std::atomic<int> wantedVolume{-1};   // set from the game loop; -1 = not changed in this game
int sentVolume = -1;                 // sender task only
int volumeBeforeMute = 50;           // game loop only

// Game loop only: the 2:1 low-pass decimator (Resample2x.h explains why not pair averages), and
// the encode buffer.
resample2x::Decimator decimator;
uint8_t encoded[1024];

/** Opens a stream for each link session (a module restart needs a new one) and sends the audio. */
void senderTask(void*) {
  uint32_t session = 0;
  uint8_t chunk[SEND_CHUNK];
  for (;;) {
    if (!AudioLink::ready()) {
      streaming = false;
      xStreamBufferReset(queue);
      vTaskDelay(pdMS_TO_TICKS(50));
      continue;
    }
    if (!streaming || AudioLink::sessionCounter() != session) {
      session = AudioLink::sessionCounter();
      streamId = static_cast<uint8_t>(streamId + 1);
      audiolink::StreamStartPayload start{};
      start.streamId = streamId.load();
      start.codec = static_cast<uint8_t>(audiolink::Codec::MuLaw);
      start.startMs = 0;
      start.sampleRate = STREAM_RATE;
      start.channels = 1;
      streaming = AudioLink::send(audiolink::Type::StreamStart, &start, sizeof(start), 500);
      if (!streaming) {
        vTaskDelay(pdMS_TO_TICKS(100));
        continue;
      }
      AudioModule::logf("Game sound: stream %u started", streamId.load());
      xStreamBufferReset(queue);  // start from now, not from what piled up meanwhile
    }
    const int wanted = wantedVolume;
    if (wanted >= 0 && wanted != sentVolume) {
      const uint8_t value = static_cast<uint8_t>(wanted);
      if (AudioLink::send(audiolink::Type::SetVolume, &value, 1, 200)) sentVolume = wanted;
    }
    const size_t got = xStreamBufferReceive(queue, chunk, sizeof(chunk), pdMS_TO_TICKS(50));
    if (got == 0) continue;
    // A short timeout: audio that cannot go out soon is better dropped than sent late.
    if (!AudioLink::sendAudio(streamId, chunk, got, 100)) streaming = false;
  }
}
#endif

}  // namespace

bool enabled() {
  SdIoMutex::Lock lock;
  FsFile file = SdMan.open(SETTING_FILE, O_RDONLY);
  if (!file) return false;
  char value = 0;
  const bool read = file.read(&value, 1) == 1;
  file.close();
  return read && value == '1';
}

void setEnabled(const bool on) {
  SdIoMutex::Lock lock;
  FsFile file = SdMan.open(SETTING_FILE, O_WRONLY | O_CREAT | O_TRUNC);
  if (!file) return;
  file.write(on ? "1" : "0", 1);
  file.close();
}

#if INX_HAS_AUDIO_MODULE

bool begin() {
  if (active_ || !enabled()) return active_;
  AudioModule::begin(true);
  const AudioModule::PowerResult power = AudioModule::setPower(true);
  // A module with its own power (or on a charger's VBUS) works without the boost.
  if (power != AudioModule::PowerResult::Ok && power != AudioModule::PowerResult::ExternalVbus) {
    INX_SERIAL.printf("[GAME] sound: module power failed (%s)\n", AudioModule::powerResultText(power));
  }
  if (!AudioModule::setLinkEnabled(true)) {
    INX_SERIAL.println("[GAME] sound: USB link did not start");
    return false;
  }
  AudioLink::begin();
  queue = xStreamBufferCreate(QUEUE_BYTES, 1);
  if (!queue ||
      xTaskCreatePinnedToCore(senderTask, "gamesound", SENDER_STACK, nullptr, SENDER_PRIORITY, nullptr, 1) != pdPASS) {
    INX_SERIAL.println("[GAME] sound: no memory for the sender");
    return false;
  }
  active_ = true;
  return true;
}

bool active() { return active_; }

int volume() {
  if (!active_) return -1;
  const int wanted = wantedVolume;
  if (wanted >= 0) return wanted;
  const AudioLink::ModuleStatus module = AudioLink::status();
  return module.valid ? module.status.volume : -1;
}

void stepVolume(const bool up) {
  const int now = volume();
  if (now < 0) return;
  const int next = now + (up ? VOLUME_STEP : -VOLUME_STEP);
  wantedVolume = next < 0 ? 0 : (next > 100 ? 100 : next);
}

void toggleMute() {
  const int now = volume();
  if (now < 0) return;
  if (now > 0) {
    volumeBeforeMute = now;
    wantedVolume = 0;
  } else {
    wantedVolume = volumeBeforeMute > 0 ? volumeBeforeMute : 50;
  }
}

void service() {
  if (active_) AudioModule::service();
}

void push(const int16_t* samples, const size_t count) {
  if (!active_ || !streaming) return;
  size_t out = 0;
  for (size_t i = 0; i < count; ++i) {
    int16_t sample = 0;
    if (!decimator.push(samples[i], sample)) continue;
    encoded[out++] = mulaw::encode(sample);
    if (out == sizeof(encoded)) {
      const size_t sent = xStreamBufferSend(queue, encoded, out, 0);
      if (sent < out) dropped = dropped + 1;
      out = 0;
    }
  }
  if (out > 0) {
    const size_t sent = xStreamBufferSend(queue, encoded, out, 0);
    if (sent < out) dropped = dropped + 1;
  }
}

const char* statusText() {
  if (!active_) return "";
  if (!streaming) return "snd -";
  // Underruns and drops since the last call (the status line's 2 s), from the module's STATUS
  // plus drops on this side; for telling a clean stream from a crackly one.
  static uint16_t lastUnderruns = 0;
  static uint16_t lastDrops = 0;
  static uint32_t lastLocalDrops = 0;
  static char text[16];
  const AudioLink::ModuleStatus module = AudioLink::status();
  if (!module.valid || module.status.streamId != streamId) return "snd ok";
  const unsigned underruns = static_cast<uint16_t>(module.status.underruns - lastUnderruns);
  const unsigned drops = static_cast<uint16_t>(module.status.decodeErrors - lastDrops) + (dropped - lastLocalDrops);
  lastUnderruns = module.status.underruns;
  lastDrops = module.status.decodeErrors;
  lastLocalDrops = dropped;
  std::snprintf(text, sizeof(text), "u%u d%u", underruns > 99 ? 99 : underruns, drops > 99 ? 99 : drops);
  return text;
}

void end() {
  if (!active_) return;
  // Ends the module's stream; the restart that follows stops the link and the boost.
  active_ = false;
  const uint8_t id = streamId;
  if (streaming) AudioLink::send(audiolink::Type::StreamStop, &id, 1, 100);
}

#else  // no audio module in this build: the setting is kept, but there is nothing to play on

bool begin() { return false; }
bool active() { return false; }
int volume() { return -1; }
void stepVolume(bool) {}
void toggleMute() {}
void service() {}
void push(const int16_t*, size_t) {}
const char* statusText() { return ""; }
void end() {}

#endif

}  // namespace GameSound

#endif  // INX_HAS_GAMES
