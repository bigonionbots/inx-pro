/**
 * @file AbsDownloader.cpp
 * @brief Definitions for the Audiobookshelf book downloader.
 */

#include "AbsDownloader.h"

#if INX_HAS_AUDIO_MODULE

#include <Arduino.h>
#include <SDCardManager.h>
#include <WiFi.h>
#include <esp_crt_bundle.h>
#include <esp_heap_caps.h>
#include <esp_http_client.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <atomic>
#include <cstdio>
#include <memory>
#include <string>

#include "AbsClient.h"
#include "AbsLocal.h"
#include "AbsWifi.h"
#include "system/AudioModule.h"
#include "util/SdIoMutex.h"

namespace AbsDownloader {

namespace {

/**
 * The card is written on the main loop only, which stalls for 1-2 s now and then (screen
 * refreshes). With 3 x 64 KB of buffers (under half a second of network data) every stall stopped
 * the download, and network and card took turns: ~300 KB/s where either alone managed ~450-500.
 * 1.5 MB of PSRAM rides out a 2-3 s stall; held only while a download runs.
 */
constexpr size_t CHUNK_BYTES = 128 * 1024;
constexpr int SLOTS = 12;
/**
 * Card writes per service() call stop after this long (at least one), so the loop stays responsive;
 * longer while the buffers are at least half full, when the card is what holds the download back.
 */
constexpr uint32_t WRITE_BUDGET_US = 200 * 1000;
constexpr uint32_t BACKLOG_WRITE_BUDGET_US = 600 * 1000;
constexpr uint32_t SPEED_WINDOW_MS = 3000;
constexpr int HTTP_TIMEOUT_MS = 15000;
/** Reconnects in a row without getting any data, before the download stops with the error. */
constexpr int MAX_RECONNECTS = 5;
constexpr uint32_t FIRST_RETRY_DELAY_MS = 1000;

/**
 * Speed diagnostics: every STATS_WINDOW_MS the module log gets one "DL" line saying where the time
 * went (network reads, waiting for a free buffer, SD writes, gaps between service() calls). Wi-Fi
 * modem sleep made no measurable difference (tried alternating it), so it stays at the default.
 * Measured on the T5 (LAN, plain HTTP or HTTPS alike): the network gives 140-550 KB/s and the card
 * takes ~410 KB/s, so ~320 KB/s overall. Every 30 s keeps the module log readable.
 */
constexpr uint32_t STATS_WINDOW_MS = 30000;

struct Slot {
  int track = 0;
  uint64_t offset = 0;
  size_t length = 0;
  uint8_t* data = nullptr;
};

Slot slots[SLOTS];
QueueHandle_t freeSlots = nullptr;
QueueHandle_t filledSlots = nullptr;
SemaphoreHandle_t lock = nullptr;  // status
TaskHandle_t task = nullptr;       // created once, then waits for start() (see downloadTask)
std::atomic<bool> stopRequested{false};
std::atomic<bool> taskRunning{false};
std::atomic<bool> taskFinished{false};  // every track fully read

AbsLocal::BookMeta meta;        // main loop
std::vector<uint64_t> startAt;  // per track: bytes already on the card when the task started
Status current;                 // guarded by lock
FsFile file;                    // main loop: the track being written
int fileTrack = -1;
uint32_t speedStartMs = 0;
uint64_t speedStartBytes = 0;

// Diagnostics (see STATS_WINDOW_MS). The task adds, the main loop takes and resets each window.
std::atomic<uint32_t> statNetUs{0};       // in stream.read()
std::atomic<uint32_t> statNetBytes{0};    // bytes those reads returned
std::atomic<uint32_t> statSlotWaitUs{0};  // waiting for a free buffer (the card is behind)
// Main loop only.
uint32_t statWindowStartMs = 0;
uint64_t statWindowStartBytes = 0;
uint32_t statSdUs = 0;
uint32_t statSdBytes = 0;
uint32_t statMaxGapMs = 0;
uint32_t statMaxBacklog = 0;
uint32_t lastServiceMs = 0;

uint32_t elapsedUs(const int64_t sinceUs) { return static_cast<uint32_t>(esp_timer_get_time() - sinceUs); }

/** Tenths of a second, for "x.y s" out of microseconds. */
unsigned tenths(const uint32_t us) { return static_cast<unsigned>((us + 50000) / 100000); }

unsigned kbPerSecond(const uint64_t bytes, const uint64_t us) {
  return us ? static_cast<unsigned>(bytes * 1000000 / us / 1024) : 0;
}

/** Main loop: one line for the window just ended. */
void logWindow(const uint64_t doneBytes) {
  const uint32_t now = millis();
  const uint32_t windowMs = now - statWindowStartMs;
  const uint32_t netUs = statNetUs.exchange(0);
  const uint32_t netBytes = statNetBytes.exchange(0);
  const uint32_t waitUs = statSlotWaitUs.exchange(0);
  const uint64_t written = doneBytes - statWindowStartBytes;
  AudioModule::logf(
      "DL %us: %u KB/s | net %u.%us busy (%u KB/s reading), buffer wait %u.%us | SD %u.%us busy (%u KB/s), "
      "loop gap max %u ms, backlog max %u/%d",
      static_cast<unsigned>((windowMs + 500) / 1000), kbPerSecond(written, static_cast<uint64_t>(windowMs) * 1000),
      tenths(netUs) / 10, tenths(netUs) % 10, kbPerSecond(netBytes, netUs), tenths(waitUs) / 10, tenths(waitUs) % 10,
      tenths(statSdUs) / 10, tenths(statSdUs) % 10, kbPerSecond(statSdBytes, statSdUs),
      static_cast<unsigned>(statMaxGapMs), static_cast<unsigned>(statMaxBacklog), SLOTS);
  statWindowStartMs = now;
  statWindowStartBytes = doneBytes;
  statSdUs = 0;
  statSdBytes = 0;
  statMaxGapMs = 0;
  statMaxBacklog = 0;
}

void setError(const std::string& error) {
  xSemaphoreTake(lock, portMAX_DELAY);
  current.failed = true;
  current.error = error;
  xSemaphoreGive(lock);
}

/**
 * One track as a single streaming GET from `offset` to the end ("Range: bytes=N-"). Streaming
 * playback fetches 128 KB ranges on fresh connections so it can seek (HttpAudioFile); a download
 * reading that way paid a TLS handshake per 128 KB, about a second through a Cloudflare tunnel,
 * and crawled at ~50 KB/s.
 */
class TrackStream {
 public:
  ~TrackStream() { close(); }

  /** Opens at `offset`. False with `error` set; `fatal` when retrying cannot help (403, 404). */
  bool open(const std::string& url, const uint64_t offset, std::string& error, bool& fatal) {
    fatal = false;
    for (int auth = 0; auth < 2; ++auth) {
      close();
      esp_http_client_config_t config = {};
      config.url = url.c_str();
      config.timeout_ms = HTTP_TIMEOUT_MS;
      config.crt_bundle_attach = esp_crt_bundle_attach;
      config.buffer_size = Abs::HTTP_RX_BUFFER;
      config.buffer_size_tx = Abs::HTTP_TX_BUFFER;
      client_ = esp_http_client_init(&config);
      if (!client_) {
        error = "HTTP client init failed";
        return false;
      }
      char range[40];
      std::snprintf(range, sizeof(range), "bytes=%llu-", static_cast<unsigned long long>(offset));
      esp_http_client_set_header(client_, "Range", range);
      esp_http_client_set_header(client_, "User-Agent", "inx-pro/" INX_VERSION);
      const std::string authorization = Abs::authorization();
      if (!authorization.empty()) esp_http_client_set_header(client_, "Authorization", authorization.c_str());
      const esp_err_t opened = esp_http_client_open(client_, 0);
      if (opened != ESP_OK) {
        describe(opened, url, error);
        return false;
      }
      const int64_t length = esp_http_client_fetch_headers(client_);
      const int status = esp_http_client_get_status_code(client_);
      if (length < 0 && status <= 0) {
        describe(ESP_FAIL, url, error);
        error = "No response from the server: " + error;
        return false;
      }
      if (status == 401 && auth == 0 && Abs::refreshAccess()) continue;  // new access token: once more
      if (status == 206) return true;
      if (status == 200) {
        // The server ignored Range and sends the whole file: skip what is already on the card.
        return offset == 0 || skip(offset, error);
      }
      error = "HTTP " + std::to_string(status);
      fatal = status == 401 || status == 403 || status == 404 || status == 416;
      return false;
    }
    error = "HTTP 401";
    fatal = true;
    return false;
  }

  /** Fills `out` (up to `length`) unless the stream ends or fails; returns the bytes read. */
  size_t read(uint8_t* out, const size_t length) {
    size_t got = 0;
    while (got < length && client_) {
      const int n = esp_http_client_read(client_, reinterpret_cast<char*>(out) + got, static_cast<int>(length - got));
      if (n <= 0) break;
      got += static_cast<size_t>(n);
    }
    return got;
  }

  void close() {
    if (!client_) return;
    esp_http_client_close(client_);
    esp_http_client_cleanup(client_);
    client_ = nullptr;
  }

 private:
  void describe(const esp_err_t err, const std::string& url, std::string& error) {
    int mbedtlsCode = 0;
    int certFlags = 0;
    const esp_err_t tlsErr = esp_http_client_get_and_clear_last_tls_error(client_, &mbedtlsCode, &certFlags);
    error = Abs::describeConnectFailure(err, tlsErr, mbedtlsCode, certFlags, url);
    INX_SERIAL.printf("[ABSDL] %s\n", error.c_str());
  }

  bool skip(uint64_t bytes, std::string& error) {
    static char scratch[2048];
    while (bytes > 0) {
      const int n =
          esp_http_client_read(client_, scratch, static_cast<int>(bytes < sizeof(scratch) ? bytes : sizeof(scratch)));
      if (n <= 0) {
        error = "connection dropped";
        return false;
      }
      bytes -= static_cast<uint64_t>(n);
    }
    return true;
  }

  esp_http_client_handle_t client_ = nullptr;
};

/** Downloads track `i` from startAt[i]; reconnects where it stopped when the connection drops. */
void downloadTrack(const size_t i, std::string& error) {
  const Abs::Track& t = meta.tracks[i];
  const std::string url = Abs::url(t.contentUrl);
  uint64_t offset = startAt[i];
  int reconnects = 0;
  uint32_t delayMs = FIRST_RETRY_DELAY_MS;
  while (offset < t.sizeBytes && !stopRequested) {
    TrackStream stream;
    bool fatal = false;
    std::string failure;
    const int64_t openStartUs = esp_timer_get_time();
    const bool opened = stream.open(url, offset, failure, fatal);
    AudioModule::logf("DL: track %u at %llu %s in %u ms", static_cast<unsigned>(i),
                      static_cast<unsigned long long>(offset), opened ? "opened" : "failed to open",
                      static_cast<unsigned>(elapsedUs(openStartUs) / 1000));
    if (opened) {
      while (offset < t.sizeBytes && !stopRequested) {
        int slotIndex;
        const int64_t waitStartUs = esp_timer_get_time();
        const bool gotSlot = xQueueReceive(freeSlots, &slotIndex, pdMS_TO_TICKS(200)) == pdTRUE;
        statSlotWaitUs += elapsedUs(waitStartUs);
        if (!gotSlot) continue;
        Slot& slot = slots[slotIndex];
        const uint64_t left = t.sizeBytes - offset;
        const size_t want = left < CHUNK_BYTES ? static_cast<size_t>(left) : CHUNK_BYTES;
        const int64_t readStartUs = esp_timer_get_time();
        const size_t n = stream.read(slot.data, want);
        statNetUs += elapsedUs(readStartUs);
        statNetBytes += static_cast<uint32_t>(n);
        if (n == 0) {
          xQueueSend(freeSlots, &slotIndex, 0);
          failure = "connection dropped";
          break;
        }
        slot.track = static_cast<int>(i);
        slot.offset = offset;
        slot.length = n;
        offset += n;
        xQueueSend(filledSlots, &slotIndex, portMAX_DELAY);
        reconnects = 0;  // progress: the next drop starts the count again
        delayMs = FIRST_RETRY_DELAY_MS;
        if (n < want) {
          failure = "connection dropped";
          break;  // a short read: the stream ended or failed; reconnect from here
        }
      }
    }
    if (offset >= t.sizeBytes || stopRequested) return;
    if (fatal || ++reconnects > MAX_RECONNECTS) {
      error = failure.empty() ? "file ended early" : failure;
      return;
    }
    INX_SERIAL.printf("[ABSDL] track %u at %llu: %s; reconnecting\n", static_cast<unsigned>(i),
                      static_cast<unsigned long long>(offset), failure.c_str());
    vTaskDelay(pdMS_TO_TICKS(delayMs));
    delayMs *= 2;
  }
}

void runDownload() {
  std::string error;
  {
    AbsWifi::Use wifi(error);
    if (wifi.ok()) AudioModule::logf("DL: Wi-Fi %d dBm", WiFi.RSSI());
    for (size_t i = 0; wifi.ok() && i < meta.tracks.size() && !stopRequested && error.empty(); ++i) {
      if (startAt[i] < meta.tracks[i].sizeBytes) downloadTrack(i, error);
    }
    if (!wifi.ok() && error.empty()) error = "Wi-Fi unavailable";
  }
  if (!error.empty() && !stopRequested) setError(error);
  taskFinished = error.empty() && !stopRequested;
  taskRunning = false;
}

/**
 * One long-lived task, stack in PSRAM, woken by start(). Creating a task per download took its
 * 12 KB stack from internal RAM, which streaming and the module link leave short of a block that
 * size ("Cannot start the download"); and a task with a PSRAM stack should not delete itself.
 */
void downloadTask(void*) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    runDownload();
  }
}

void closeFile() {
  if (fileTrack >= 0) file.close();
  fileTrack = -1;
}

bool writeSlot(const Slot& slot) {
  SdIoMutex::Lock sdLock;
  if (fileTrack != slot.track) {
    closeFile();
    const std::string path = meta.folder + "/" + meta.tracks[slot.track].filename;
    file = SdMan.open(path.c_str(), O_RDWR | O_CREAT);
    if (!file) return false;
    fileTrack = slot.track;
  }
  if (!file.seekSet(slot.offset)) return false;
  return file.write(slot.data, slot.length) == slot.length;
}

/** The buffers, in PSRAM, while a download runs (start() and service()). Main loop. */
bool allocateSlots() {
  for (Slot& slot : slots) {
    if (!slot.data)
      slot.data = static_cast<uint8_t*>(heap_caps_malloc(CHUNK_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!slot.data) return false;
  }
  return true;
}

/** Only once the task is idle and every filled buffer is on the card. Main loop. */
void freeSlotBuffers() {
  for (Slot& slot : slots) {
    heap_caps_free(slot.data);
    slot.data = nullptr;
  }
}

/** Free memory, for an error that says why something could not be allocated. */
std::string memoryNote() {
  char text[96];
  snprintf(text, sizeof(text), "internal %u KB free (largest %u KB), PSRAM %u KB free",
           static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
           static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024),
           static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
  return text;
}

}  // namespace

bool begin(std::string& error) {
  if (!lock) lock = xSemaphoreCreateMutex();
  if (!freeSlots) freeSlots = xQueueCreate(SLOTS, sizeof(int));
  if (!filledSlots) filledSlots = xQueueCreate(SLOTS, sizeof(int));
  bool ok = lock && freeSlots && filledSlots;
  // The task's stack is in PSRAM, but its control block must be internal RAM, which a long
  // session (streaming, the module link, TLS) can leave without even that: create it at boot.
  if (ok && !task &&
      xTaskCreatePinnedToCoreWithCaps(downloadTask, "abs_download", 12288, nullptr, 2, &task, tskNO_AFFINITY,
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
    task = nullptr;
    ok = false;
  }
  if (!ok) {
    error = "Cannot start the download: out of memory (" + memoryNote() + ")";
    INX_SERIAL.printf("[ABSDL] %s\n", error.c_str());
  }
  return ok;
}

bool start(const Abs::ItemDetail& item, std::string& error) {
  // A task told to stop finishes its current chunk first; wait for it rather than run two.
  for (int i = 0; i < 100 && taskRunning && stopRequested; ++i) vTaskDelay(pdMS_TO_TICKS(50));
  if (taskRunning) return true;
  if (!begin(error)) return false;  // normally done at boot (AbsQueue::begin())
  if (!allocateSlots()) {
    freeSlotBuffers();
    error = "Cannot start the download: out of memory (" + memoryNote() + ")";
    INX_SERIAL.printf("[ABSDL] %s\n", error.c_str());
    return false;
  }
  xQueueReset(freeSlots);
  xQueueReset(filledSlots);
  for (int i = 0; i < SLOTS; ++i) xQueueSend(freeSlots, &i, 0);

  // Same folder as an earlier, interrupted download of this item; otherwise a new one.
  AbsLocal::BookMeta existing;
  const bool resume = AbsLocal::find(item.id, existing);
  meta = AbsLocal::BookMeta{};
  // An episode's folder carries its podcast's name, so episodes sort together under /Audiobooks.
  const bool episode = Abs::isEpisode(item.id);
  meta.folder = resume ? existing.folder : AbsLocal::newFolder(episode ? item.author + " - " + item.title : item.title);
  meta.itemId = item.id;
  meta.title = item.title;
  meta.author = item.author;
  meta.durationMs = item.durationMs;
  meta.complete = false;
  for (Abs::Track track : item.tracks) {
    track.filename = AbsLocal::localName(track);
    meta.tracks.push_back(track);
  }
  meta.chapters = item.chapters;
  if (!AbsLocal::write(meta)) {
    error = "Cannot write to the SD card";
    return false;
  }

  Status fresh;
  fresh.active = true;
  fresh.itemId = item.id;
  fresh.title = item.title;
  fresh.trackCount = static_cast<int>(meta.tracks.size());
  startAt.clear();
  for (const Abs::Track& track : meta.tracks) {
    uint64_t have = AbsLocal::localSize(meta, track);
    if (have > track.sizeBytes) {
      // Not the file we expect (re-encoded on the server?): start that track over.
      SdIoMutex::Lock sdLock;
      SdMan.remove((meta.folder + "/" + track.filename).c_str());
      have = 0;
    }
    startAt.push_back(have);
    fresh.doneBytes += have;
    fresh.totalBytes += track.sizeBytes;
  }
  xSemaphoreTake(lock, portMAX_DELAY);
  current = fresh;
  xSemaphoreGive(lock);
  speedStartMs = millis();
  speedStartBytes = fresh.doneBytes;
  statNetUs = 0;
  statNetBytes = 0;
  statSlotWaitUs = 0;
  statWindowStartMs = millis();
  statWindowStartBytes = fresh.doneBytes;
  statSdUs = 0;
  statSdBytes = 0;
  statMaxGapMs = 0;
  statMaxBacklog = 0;
  lastServiceMs = 0;

  stopRequested = false;
  taskFinished = false;
  taskRunning = true;
  xTaskNotifyGive(task);
  return true;
}

void pause() {
  stopRequested = true;
  if (!lock) return;
  // Chunks still in the buffers are dropped; start() fetches from what is on the card.
  {
    SdIoMutex::Lock sdLock;
    closeFile();
  }
  xSemaphoreTake(lock, portMAX_DELAY);
  current.active = false;
  xSemaphoreGive(lock);
}

void service() {
  if (!lock) return;
  if (taskRunning) {
    const uint32_t now = millis();
    if (lastServiceMs && now - lastServiceMs > statMaxGapMs) statMaxGapMs = now - lastServiceMs;
    lastServiceMs = now;
    const uint32_t backlog = uxQueueMessagesWaiting(filledSlots);
    if (backlog > statMaxBacklog) statMaxBacklog = backlog;
  }
  const int64_t serviceStartUs = esp_timer_get_time();
  const uint32_t budgetUs =
      uxQueueMessagesWaiting(filledSlots) >= SLOTS / 2 ? BACKLOG_WRITE_BUDGET_US : WRITE_BUDGET_US;
  while (elapsedUs(serviceStartUs) < budgetUs) {
    int slotIndex;
    if (xQueueReceive(filledSlots, &slotIndex, 0) != pdTRUE) break;
    const Slot& slot = slots[slotIndex];
    const int64_t writeStartUs = esp_timer_get_time();
    const bool ok = writeSlot(slot);
    statSdUs += elapsedUs(writeStartUs);
    statSdBytes += static_cast<uint32_t>(slot.length);
    xQueueSend(freeSlots, &slotIndex, 0);
    if (!ok) {
      stopRequested = true;
      setError("SD card write failed (full?)");
      break;
    }
    xSemaphoreTake(lock, portMAX_DELAY);
    current.doneBytes += slot.length;
    current.track = slot.track;
    const uint32_t elapsed = millis() - speedStartMs;
    if (elapsed >= SPEED_WINDOW_MS) {
      current.bytesPerSecond = static_cast<uint32_t>((current.doneBytes - speedStartBytes) * 1000 / elapsed);
      speedStartMs = millis();
      speedStartBytes = current.doneBytes;
    }
    xSemaphoreGive(lock);
  }
  if (taskRunning && millis() - statWindowStartMs >= STATS_WINDOW_MS) {
    xSemaphoreTake(lock, portMAX_DELAY);
    const uint64_t doneBytes = current.doneBytes;
    xSemaphoreGive(lock);
    logWindow(doneBytes);
  }

  // Wrap up once the task is gone and every buffer is on the card.
  xSemaphoreTake(lock, portMAX_DELAY);
  const bool wasActive = current.active;
  xSemaphoreGive(lock);
  // Paused or finished, and nothing left to write: give the 1.5 MB back.
  if (!wasActive && !taskRunning && slots[0].data && uxQueueMessagesWaiting(filledSlots) == 0) freeSlotBuffers();
  if (!wasActive || taskRunning || uxQueueMessagesWaiting(filledSlots) > 0) return;
  {
    SdIoMutex::Lock sdLock;
    closeFile();
  }
  if (taskFinished) {
    meta.complete = true;
    AbsLocal::write(meta);
  }
  xSemaphoreTake(lock, portMAX_DELAY);
  current.active = false;
  current.done = taskFinished;
  xSemaphoreGive(lock);
}

Status status() {
  if (!lock) return {};
  xSemaphoreTake(lock, portMAX_DELAY);
  Status copy = current;
  xSemaphoreGive(lock);
  return copy;
}

bool active() {
  if (!lock) return false;
  xSemaphoreTake(lock, portMAX_DELAY);
  const bool running = current.active;
  xSemaphoreGive(lock);
  return running;
}

}  // namespace AbsDownloader

#endif  // INX_HAS_AUDIO_MODULE
