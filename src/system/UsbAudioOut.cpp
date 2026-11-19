/**
 * @file UsbAudioOut.cpp
 * @brief Definitions for USB headphone adapter output.
 */

#include "UsbAudioOut.h"

#if INX_HAS_AUDIO_MODULE

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <usb/uac_host.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "AudioModule.h"

namespace UsbAudioOut {

namespace {

constexpr uint32_t TONE_RATE = 44100;
constexpr uint8_t CHANNELS = 2;
constexpr uint8_t BITS = 16;
/**
 * The driver's PCM ring buffer: about 45 ms of 44.1 kHz stereo, plus 16 ms in transfers. The
 * driver creates it with xRingbufferCreate, which takes internal RAM for as long as the adapter is
 * plugged in, and HTTPS needs 16 KB blocks of that (a 16 KB ring made streamed books' TLS fail).
 */
constexpr uint32_t RING_BYTES = 8 * 1024;
constexpr uint32_t RING_THRESHOLD = 2 * 1024;
/** 10 ms per tone write. */
constexpr uint32_t CHUNK_FRAMES = 480;
/** The most one driver write takes: half its buffer, so a write never waits for all of it. */
constexpr size_t MAX_WRITE_BYTES = RING_BYTES / 2;
constexpr uint32_t WRITE_TIMEOUT_MS = 200;
/** Tone segments: left, right, both. */
constexpr uint32_t SEGMENT_MS = 1000;
constexpr float TONE_HZ[3] = {440.0f, 660.0f, 550.0f};
/** -20 dBFS: audible but not loud, whatever the headphones. */
constexpr float TONE_LEVEL = 0.1f;
/** The tone's adapter volume, normalised (0-100) across the adapter's own range. */
constexpr uint8_t TONE_VOLUME = 60;
constexpr uint32_t FADE_FRAMES = 441;  ///< 10 ms ramps, so the segments do not click

constexpr UBaseType_t DRIVER_TASK_PRIORITY = 5;  // same as the USB host task
constexpr UBaseType_t AUDIO_TASK_PRIORITY = 4;
constexpr uint32_t DRIVER_STACK_BYTES = 4096;
constexpr uint32_t AUDIO_STACK_BYTES = 4096;
constexpr UBaseType_t AUDIO_STACK_CAPS = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
constexpr uint32_t STOP_TIMEOUT_MS = 3000;

constexpr uint32_t NOTIFY_CONNECTED = 1u << 0;
constexpr uint32_t NOTIFY_DISCONNECTED = 1u << 1;
constexpr uint32_t NOTIFY_TONE = 1u << 2;
constexpr uint32_t NOTIFY_STOP = 1u << 3;

SemaphoreHandle_t lock = nullptr;  // guards connectedDevice and pending*
DeviceInfo connectedDevice;
uint8_t pendingAddr = 0;
uint8_t pendingIface = 0;
bool pendingValid = false;

/** Guards handle and the stream state: the stream calls come from other tasks. */
SemaphoreHandle_t ioLock = nullptr;
uac_host_device_handle_t handle = nullptr;
/** Written under ioLock, read without it (status polling must not wait behind a write). */
std::atomic<uint32_t> runningRate{0};
uint8_t volumePercent = 50;
bool toneRunning = false;
std::atomic<uint32_t> opens{0};
TaskHandle_t audioTask = nullptr;
SemaphoreHandle_t audioDone = nullptr;
bool driverInstalled = false;

std::atomic<State> currentState{State::Off};
std::atomic<bool> stopping{false};
std::atomic<uint32_t> tones{0};
std::atomic<uint32_t> lastToneBytes{0};
std::atomic<uint32_t> lastToneMs{0};
std::atomic<uint32_t> writeTimeouts{0};
std::atomic<uint32_t> transferErrors{0};

class LockGuard {
 public:
  LockGuard() { xSemaphoreTake(lock, portMAX_DELAY); }
  ~LockGuard() { xSemaphoreGive(lock); }
  LockGuard(const LockGuard&) = delete;
  LockGuard& operator=(const LockGuard&) = delete;
};

class IoGuard {
 public:
  IoGuard() { xSemaphoreTake(ioLock, portMAX_DELAY); }
  ~IoGuard() { xSemaphoreGive(ioLock); }
  IoGuard(const IoGuard&) = delete;
  IoGuard& operator=(const IoGuard&) = delete;
};

void copyWide(const wchar_t* in, char* out, const size_t outSize) {
  size_t i = 0;
  for (; in[i] && i + 1 < outSize; ++i) {
    const wchar_t c = in[i];
    out[i] = (c >= 0x20 && c < 0x7F) ? static_cast<char>(c) : '?';
  }
  out[i] = '\0';
}

// UAC driver task: hand the work to the audio task.
void onDriverEvent(const uint8_t addr, const uint8_t ifaceNum, const uac_host_driver_event_t event, void*) {
  if (event == UAC_HOST_DRIVER_EVENT_RX_CONNECTED) {
    AudioModule::logf("Headphones: microphone input on interface %u (not used)", ifaceNum);
    return;
  }
  if (event != UAC_HOST_DRIVER_EVENT_TX_CONNECTED) return;
  {
    LockGuard guard;
    pendingAddr = addr;
    pendingIface = ifaceNum;
    pendingValid = true;
  }
  if (audioTask) xTaskNotify(audioTask, NOTIFY_CONNECTED, eSetBits);
}

void onDeviceEvent(uac_host_device_handle_t, const uac_host_device_event_t event, void*) {
  switch (event) {
    case UAC_HOST_DRIVER_EVENT_DISCONNECTED:
      if (audioTask) xTaskNotify(audioTask, NOTIFY_DISCONNECTED, eSetBits);
      break;
    case UAC_HOST_DEVICE_EVENT_TRANSFER_ERROR:
      ++transferErrors;
      break;
    default:
      break;
  }
}

/** Logs every output alternate setting, keeps a one-line summary and the 16-bit stereo rates. */
void describeFormats(const uint8_t altCount, DeviceInfo& dev) {
  char* summary = dev.formats;
  const size_t summarySize = sizeof(dev.formats);
  bool bits16 = false;
  bool bits24 = false;
  uint8_t channels = 0;
  uint32_t rates[8] = {};
  size_t rateCount = 0;
  for (uint8_t alt = 1; alt <= altCount; ++alt) {
    uac_host_dev_alt_param_t param{};
    if (uac_host_get_device_alt_param(handle, alt, &param) != ESP_OK) continue;
    char line[96];
    int n = snprintf(line, sizeof(line), "Headphones: alt %u: %u ch, %u-bit (%u-byte)", alt, param.channels,
                     param.bit_resolution, param.subframe_size);
    if (param.sample_freq_type == 0) {
      snprintf(line + n, sizeof(line) - n, ", %lu-%lu Hz", static_cast<unsigned long>(param.sample_freq_lower),
               static_cast<unsigned long>(param.sample_freq_upper));
    } else {
      for (uint8_t i = 0; i < param.sample_freq_type && i < UAC_FREQ_NUM_MAX && n < static_cast<int>(sizeof(line));
           ++i) {
        n += snprintf(line + n, sizeof(line) - n, "%s%lu", i == 0 ? ", " : "/",
                      static_cast<unsigned long>(param.sample_freq[i]));
        bool known = false;
        for (size_t r = 0; r < rateCount; ++r) known = known || rates[r] == param.sample_freq[i];
        if (!known && rateCount < 8) rates[rateCount++] = param.sample_freq[i];
        if (param.channels == CHANNELS && param.bit_resolution == BITS && dev.rateCount < 8) {
          dev.rates[dev.rateCount++] = param.sample_freq[i];
        }
      }
    }
    AudioModule::logf("%s", line);
    bits16 = bits16 || param.bit_resolution == 16;
    bits24 = bits24 || param.bit_resolution == 24;
    if (param.channels > channels) channels = param.channels;
  }
  for (size_t i = 1; i < rateCount; ++i) {
    for (size_t j = i; j > 0 && rates[j - 1] > rates[j]; --j) std::swap(rates[j - 1], rates[j]);
  }
  for (size_t i = 1; i < dev.rateCount; ++i) {
    for (size_t j = i; j > 0 && dev.rates[j - 1] > dev.rates[j]; --j) std::swap(dev.rates[j - 1], dev.rates[j]);
  }
  int n = snprintf(summary, summarySize, "%s%s%s-bit %uch", bits16 ? "16" : "", bits16 && bits24 ? "/" : "",
                   bits24 ? "24" : "", channels);
  for (size_t i = 0; i < rateCount && n < static_cast<int>(summarySize); ++i) {
    n += snprintf(summary + n, summarySize - n, " %lu", static_cast<unsigned long>(rates[i]));
  }
}

void closeAdapter(const char* reason) {
  {
    IoGuard io;
    if (!handle) return;
    uac_host_device_close(handle);
    handle = nullptr;
    runningRate = 0;
  }
  {
    LockGuard guard;
    connectedDevice = DeviceInfo{};
  }
  if (!stopping) currentState = State::Waiting;
  AudioModule::logf("Headphones: adapter closed (%s)", reason);
}

void openPending() {
  uint8_t addr = 0;
  uint8_t ifaceNum = 0;
  {
    LockGuard guard;
    if (!pendingValid) return;
    addr = pendingAddr;
    ifaceNum = pendingIface;
    pendingValid = false;
  }
  closeAdapter("replaced");

  uac_host_device_config_t config{};
  config.addr = addr;
  config.iface_num = ifaceNum;
  config.buffer_size = RING_BYTES;
  config.buffer_threshold = RING_THRESHOLD;
  config.callback = onDeviceEvent;
  config.callback_arg = nullptr;
  uac_host_device_handle_t opened = nullptr;
  esp_err_t err = uac_host_device_open(&config, &opened);
  if (err != ESP_OK) {
    AudioModule::logf("Headphones: open addr %u iface %u failed: %s", addr, ifaceNum, esp_err_to_name(err));
    return;
  }
  {
    IoGuard io;
    handle = opened;
  }

  uac_host_dev_info_t info{};
  DeviceInfo dev;
  if (uac_host_get_device_info(handle, &info) == ESP_OK) {
    dev.vid = info.VID;
    dev.pid = info.PID;
    copyWide(info.iProduct, dev.product, sizeof(dev.product));
  }
  AudioModule::logf("Headphones: %04X:%04X \"%s\", output interface %u, %u formats", dev.vid, dev.pid, dev.product,
                    ifaceNum, info.iface_alt_num);
  describeFormats(info.iface_alt_num, dev);
  if (dev.rateCount == 0) AudioModule::logf("Headphones: no 16-bit stereo output, cannot play");
  {
    LockGuard guard;
    connectedDevice = dev;
  }
  currentState = State::Ready;
  ++opens;
}

/** Applies volumePercent to the running stream (ioLock held). */
void applyVolumeLocked() {
  if (!handle || runningRate == 0) return;
  if (volumePercent == 0) {
    uac_host_device_set_mute(handle, true);
    return;
  }
  uac_host_device_set_mute(handle, false);
  const esp_err_t err = uac_host_device_set_volume(handle, volumePercent);
  if (err != ESP_OK) AudioModule::logf("Headphones: no volume control (%s)", esp_err_to_name(err));
}

/** One segment of the tone into the driver: left (0), right (1) or both (2). False when cut short. */
bool playSegment(const int segment, const uint32_t rate, int16_t* chunk, float& phase, uint32_t& bytes) {
  const uint32_t frames = rate * SEGMENT_MS / 1000;
  const float step = 2.0f * static_cast<float>(M_PI) * TONE_HZ[segment] / static_cast<float>(rate);
  for (uint32_t done = 0; done < frames;) {
    if (stopping) return false;
    const uint32_t n = std::min(CHUNK_FRAMES, frames - done);
    for (uint32_t i = 0; i < n; ++i) {
      const uint32_t f = done + i;
      float gain = TONE_LEVEL;
      if (f < FADE_FRAMES) gain *= static_cast<float>(f) / FADE_FRAMES;
      if (frames - f < FADE_FRAMES) gain *= static_cast<float>(frames - f) / FADE_FRAMES;
      const auto sample = static_cast<int16_t>(sinf(phase) * gain * 32767.0f);
      phase += step;
      if (phase > 2.0f * static_cast<float>(M_PI)) phase -= 2.0f * static_cast<float>(M_PI);
      chunk[2 * i] = segment == 1 ? 0 : sample;
      chunk[2 * i + 1] = segment == 0 ? 0 : sample;
    }
    if (streamWrite(chunk, n, WRITE_TIMEOUT_MS) != n) {
      AudioModule::logf("Headphones: the adapter stopped taking audio");
      return false;
    }
    bytes += n * CHANNELS * sizeof(int16_t);
    done += n;
  }
  return true;
}

void playTone() {
  const uint32_t rate = bestRate(TONE_RATE);
  uint8_t savedVolume = 0;
  {
    IoGuard io;
    if (!handle || runningRate != 0) return;
    toneRunning = true;
    savedVolume = volumePercent;
    volumePercent = TONE_VOLUME;
  }
  if (rate == 0 || !streamStart(rate)) {
    IoGuard io;
    toneRunning = false;
    volumePercent = savedVolume;
    return;
  }
  int16_t volumeDb = 0;
  {
    IoGuard io;
    if (handle && uac_host_device_get_volume_db(handle, &volumeDb) == ESP_OK) {
      // UAC reports volume in 1/256 dB.
      AudioModule::logf("Headphones: volume %u%% (%.1f dB)", TONE_VOLUME, volumeDb / 256.0);
    }
  }

  auto* chunk = static_cast<int16_t*>(heap_caps_malloc(CHUNK_FRAMES * CHANNELS * sizeof(int16_t), MALLOC_CAP_INTERNAL));
  uint32_t bytes = 0;
  const uint32_t startMs = millis();
  bool complete = chunk != nullptr;
  float phase = 0.0f;
  for (int segment = 0; complete && segment < 3; ++segment) {
    complete = playSegment(segment, rate, chunk, phase, bytes);
  }
  // The last writes are still in the ring buffer: let them play out before stopping the stream.
  if (complete) vTaskDelay(pdMS_TO_TICKS(streamBufferedMs() + 20));
  const uint32_t elapsed = millis() - startMs;
  heap_caps_free(chunk);
  streamStop();
  {
    IoGuard io;
    toneRunning = false;
    volumePercent = savedVolume;
  }

  lastToneBytes = bytes;
  lastToneMs = elapsed;
  ++tones;
  const uint32_t expectedMs = static_cast<uint32_t>(bytes / (rate * CHANNELS * 2.0f / 1000.0f));
  AudioModule::logf("Headphones: tone %s at %lu Hz, %lu bytes in %lu ms (audio %lu ms), %lu transfer errors",
                    complete ? "done" : "cut short", static_cast<unsigned long>(rate),
                    static_cast<unsigned long>(bytes), static_cast<unsigned long>(elapsed),
                    static_cast<unsigned long>(expectedMs), static_cast<unsigned long>(transferErrors.load()));
}

void audioTaskMain(void*) {
  for (;;) {
    uint32_t bits = 0;
    xTaskNotifyWait(0, UINT32_MAX, &bits, portMAX_DELAY);
    if (stopping || (bits & NOTIFY_STOP)) break;
    if (bits & NOTIFY_DISCONNECTED) closeAdapter("unplugged");
    if (bits & NOTIFY_CONNECTED) openPending();
    if (bits & NOTIFY_TONE) playTone();
  }
  closeAdapter("link stopped");
  xSemaphoreGive(audioDone);
  vTaskSuspend(nullptr);  // a PSRAM-stack task cannot delete itself: uninstall() deletes it
}

}  // namespace

bool install() {
  if (driverInstalled) return true;
  if (!lock) lock = xSemaphoreCreateMutex();
  if (!ioLock) ioLock = xSemaphoreCreateMutex();
  if (!audioDone) audioDone = xSemaphoreCreateBinary();
  if (!lock || !ioLock || !audioDone) {
    currentState = State::Failed;
    return false;
  }
  stopping = false;
  xSemaphoreTake(audioDone, 0);
  {
    LockGuard guard;
    pendingValid = false;
  }

  // The task first: the driver reports an adapter as soon as the root port is powered.
  if (xTaskCreatePinnedToCoreWithCaps(audioTaskMain, "usb_audio", AUDIO_STACK_BYTES, nullptr, AUDIO_TASK_PRIORITY,
                                      &audioTask, tskNO_AFFINITY, AUDIO_STACK_CAPS) != pdPASS) {
    audioTask = nullptr;
    AudioModule::logf("Headphones: task create failed");
    currentState = State::Failed;
    return false;
  }

  uac_host_driver_config_t config{};
  config.create_background_task = true;
  config.task_priority = DRIVER_TASK_PRIORITY;
  config.stack_size = DRIVER_STACK_BYTES;
  config.core_id = tskNO_AFFINITY;
  config.callback = onDriverEvent;
  config.callback_arg = nullptr;
  const esp_err_t err = uac_host_install(&config);
  if (err != ESP_OK) {
    AudioModule::logf("Headphones: UAC driver install failed: %s", esp_err_to_name(err));
    stopping = true;
    xTaskNotify(audioTask, NOTIFY_STOP, eSetBits);
    if (xSemaphoreTake(audioDone, pdMS_TO_TICKS(STOP_TIMEOUT_MS)) == pdTRUE) {
      vTaskDeleteWithCaps(audioTask);
      audioTask = nullptr;
    }
    stopping = false;
    currentState = State::Failed;
    return false;
  }
  driverInstalled = true;
  currentState = State::Waiting;
  return true;
}

void uninstall() {
  if (!driverInstalled) {
    if (currentState == State::Failed) currentState = State::Off;
    return;
  }
  stopping = true;
  if (audioTask) {
    xTaskNotify(audioTask, NOTIFY_STOP, eSetBits);
    if (xSemaphoreTake(audioDone, pdMS_TO_TICKS(STOP_TIMEOUT_MS)) != pdTRUE) {
      AudioModule::logf("Headphones: task did not stop");
    } else {
      for (int i = 0; i < 100 && eTaskGetState(audioTask) != eSuspended; ++i) vTaskDelay(pdMS_TO_TICKS(2));
      vTaskDeleteWithCaps(audioTask);
      audioTask = nullptr;
    }
  }
  const esp_err_t err = uac_host_uninstall();
  if (err != ESP_OK) AudioModule::logf("Headphones: UAC driver uninstall failed: %s", esp_err_to_name(err));
  driverInstalled = false;
  stopping = false;
  currentState = State::Off;
}

State state() { return currentState.load(); }

bool isConnected() {
  const State now = currentState.load();
  return now == State::Ready || now == State::Playing;
}

DeviceInfo device() {
  if (!lock) return DeviceInfo{};
  LockGuard guard;
  return connectedDevice;
}

Counters counters() {
  Counters out;
  out.tones = tones.load();
  out.lastToneBytes = lastToneBytes.load();
  out.lastToneMs = lastToneMs.load();
  out.writeTimeouts = writeTimeouts.load();
  out.transferErrors = transferErrors.load();
  return out;
}

bool playTestTone() {
  if (!isConnected() || !audioTask || streamRate() != 0) return false;
  xTaskNotify(audioTask, NOTIFY_TONE, eSetBits);
  return true;
}

uint32_t connectCounter() { return opens.load(); }

uint32_t bestRate(const uint32_t sourceRate) {
  const DeviceInfo dev = device();
  if (dev.rateCount == 0) return 0;
  for (uint8_t i = 0; i < dev.rateCount; ++i) {
    if (dev.rates[i] == sourceRate) return sourceRate;
  }
  // The 44.1 kHz family (11025, 22050, 88200) to 44.1 kHz, everything else to 48 kHz: whole
  // ratios where possible.
  const uint32_t preferred = sourceRate % 11025 == 0 ? 44100 : 48000;
  for (uint8_t i = 0; i < dev.rateCount; ++i) {
    if (dev.rates[i] == preferred) return preferred;
  }
  for (uint8_t i = 0; i < dev.rateCount; ++i) {
    if (dev.rates[i] == 44100 || dev.rates[i] == 48000) return dev.rates[i];
  }
  return dev.rates[dev.rateCount - 1];
}

bool streamStart(const uint32_t rate) {
  if (!ioLock) return false;
  IoGuard io;
  if (!handle) return false;
  if (runningRate == rate) return true;
  if (runningRate != 0) {
    uac_host_device_stop(handle);
    runningRate = 0;
  }
  uac_host_stream_config_t stream{};
  stream.channels = CHANNELS;
  stream.bit_resolution = BITS;
  stream.sample_freq = rate;
  const esp_err_t err = uac_host_device_start(handle, &stream);
  if (err != ESP_OK) {
    AudioModule::logf("Headphones: start %lu Hz failed: %s", static_cast<unsigned long>(rate), esp_err_to_name(err));
    return false;
  }
  runningRate = rate;
  applyVolumeLocked();
  currentState = State::Playing;
  return true;
}

size_t streamWrite(const int16_t* frames, const size_t count, const uint32_t timeoutMs) {
  if (!ioLock || count == 0) return 0;
  IoGuard io;
  if (!handle || runningRate == 0) return 0;
  const auto* bytes = reinterpret_cast<const uint8_t*>(frames);
  const size_t total = count * CHANNELS * sizeof(int16_t);
  size_t written = 0;
  while (written < total) {
    const size_t n = std::min(total - written, MAX_WRITE_BYTES);
    const esp_err_t err =
        uac_host_device_write(handle, const_cast<uint8_t*>(bytes + written), n, pdMS_TO_TICKS(timeoutMs));
    if (err != ESP_OK) {
      if (err == ESP_ERR_TIMEOUT) ++writeTimeouts;
      break;
    }
    written += n;
  }
  return written / (CHANNELS * sizeof(int16_t));
}

void streamStop() {
  if (!ioLock) return;
  IoGuard io;
  if (handle && runningRate != 0) uac_host_device_stop(handle);
  runningRate = 0;
  if (handle && !stopping) currentState = State::Ready;
}

uint32_t streamRate() { return runningRate.load(); }

uint32_t streamBufferedMs() {
  const uint32_t rate = streamRate();
  // The driver does not report its fill; while playing it is kept full, so this is the most.
  return rate ? static_cast<uint32_t>(RING_BYTES * 1000ull / (rate * CHANNELS * sizeof(int16_t))) : 0;
}

bool setVolume(const uint8_t percent) {
  if (!ioLock) return false;
  IoGuard io;
  if (toneRunning) return false;
  volumePercent = percent > 100 ? 100 : percent;
  applyVolumeLocked();
  return handle && runningRate != 0;
}

}  // namespace UsbAudioOut

#endif  // INX_HAS_AUDIO_MODULE
