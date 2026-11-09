/**
 * @file AudioPlayer.cpp
 * @brief Definitions for the audiobook player.
 */

#include "AudioPlayer.h"

#if INX_HAS_AUDIO_MODULE

#include <Arduino.h>
#include <ArduinoJson.h>
#include <SDCardManager.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <deque>
#include <iterator>
#include <memory>

#include "AudioLink.h"
#include "AudioModule.h"
#include "abs/AbsClient.h"
#include "abs/AbsCovers.h"
#include "abs/AbsLocal.h"
#include "abs/AbsWifi.h"
#include "audio/AudioSource.h"
#include "audio/HttpAudioFile.h"
#include "util/SdIoMutex.h"

namespace AudioPlayer {

namespace {

using audiolink::PlayState;
using audiolink::Type;

constexpr const char* PROGRESS_FILE = "/.system/audio_progress.txt";
constexpr size_t QUEUE_BYTES = 512 * 1024;
constexpr size_t DATA_CHUNK = audiolink::MAX_PAYLOAD - 1;  // AUDIO_DATA carries the stream id first
constexpr int REFILL_RECORDS_PER_SERVICE = 16;
constexpr uint32_t SAVE_INTERVAL_MS = 30000;
constexpr uint32_t MODULE_RETRY_MS = 5000;
constexpr uint32_t CONTROL_TIMEOUT_MS = 300;
constexpr uint32_t PREVIOUS_RESTARTS_TRACK_MS = 5000;
constexpr uint32_t STREAM_RETRY_MS = 5000;
/**
 * Progress pushes while playing an Audiobookshelf book: a streamed one every 15 s (Wi-Fi is on
 * anyway); a downloaded one every minute, but only while Wi-Fi happens to be on, so playback
 * never turns the radio on just for this (pause, track changes and stop still push, connecting
 * if need be).
 */
constexpr uint32_t REMOTE_PUSH_INTERVAL_MS = 15000;
constexpr uint32_t LOCAL_PUSH_INTERVAL_MS = 60000;
/** Resuming after a pause this long first asks the server: another device may have moved on. */
constexpr uint32_t RESUME_FETCH_AFTER_MS = 60000;
/**
 * A streamed book paused this long lets go of Wi-Fi (the stream stops; the position is kept), so a
 * forgotten pause does not keep the radio on. Play reconnects and restarts the stream there.
 */
constexpr uint32_t PAUSED_WIFI_OFF_MS = 3 * 60 * 1000;
constexpr const char* WIFI_PARKED_MESSAGE = "Wi-Fi off while paused";
/** How long resumeAfterScreen() keeps the Wi-Fi it was handed, for the stream to take over. */
constexpr uint32_t HANDED_WIFI_HOLD_MS = 20000;
// The feeder keeps Wi-Fi this long after its last stream ends, so a skip, a seek or the next
// chapter reuses the connection instead of turning the radio off and joining again.
constexpr uint32_t FEED_WIFI_LINGER_MS = 20000;
/** While streaming, a log line this often on how the stream keeps up (see logStreamHealth()). */
constexpr uint32_t STREAM_HEALTH_LOG_MS = 20000;
constexpr const char* WIFI_RECONNECTING_MESSAGE = "Reconnecting Wi-Fi...";
constexpr uint32_t SYNC_TOLERANCE_MS = 2000;
constexpr const char* ABS_KEY_PREFIX = "abs:";

// ---------------------------------------------------------------------------------------------
// Queue between service() (producer, main loop) and the sender task (consumer).

enum class RecordKind : uint8_t { Start, Data, End };

struct RecordHeader {
  RecordKind kind;
  uint8_t streamId;
  uint16_t length;
};

struct Record {
  RecordHeader header;
  uint32_t generation;
  uint8_t data[audiolink::MAX_PAYLOAD];
};

class FeedQueue {
 public:
  bool begin() {
    buffer_ = static_cast<uint8_t*>(heap_caps_malloc(QUEUE_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    lock_ = xSemaphoreCreateMutex();
    return buffer_ && lock_;
  }

  /**
   * Appends a record. With a generation, a queue cleared since then drops it silently: the
   * network feeder checks for that only between reads, and clear() can land in between.
   */
  bool push(const RecordKind kind, const uint8_t streamId, const uint8_t* data, const uint16_t length,
            const uint32_t generation = UINT32_MAX) {
    const RecordHeader header{kind, streamId, length};
    xSemaphoreTake(lock_, portMAX_DELAY);
    if (generation != UINT32_MAX && generation != generation_) {
      xSemaphoreGive(lock_);
      return true;
    }
    const bool fits = QUEUE_BYTES - used_ >= sizeof(header) + length;
    if (fits) {
      write(reinterpret_cast<const uint8_t*>(&header), sizeof(header));
      write(data, length);
    }
    xSemaphoreGive(lock_);
    return fits;
  }

  bool pop(Record& out) {
    xSemaphoreTake(lock_, portMAX_DELAY);
    const bool any = used_ >= sizeof(RecordHeader);
    if (any) {
      read(reinterpret_cast<uint8_t*>(&out.header), sizeof(out.header));
      read(out.data, out.header.length);
      out.generation = generation_;
    }
    xSemaphoreGive(lock_);
    return any;
  }

  /** Drops everything queued; records already popped are recognised as stale by their generation. */
  void clear() {
    xSemaphoreTake(lock_, portMAX_DELAY);
    head_ = tail_ = used_ = 0;
    ++generation_;
    xSemaphoreGive(lock_);
  }

  size_t freeBytes() const { return QUEUE_BYTES - used_; }
  size_t usedBytes() const { return used_; }
  uint32_t generation() const { return generation_; }

 private:
  void write(const uint8_t* data, size_t length) {
    while (length > 0) {
      const size_t run = std::min(length, QUEUE_BYTES - head_);
      std::memcpy(buffer_ + head_, data, run);
      head_ = (head_ + run) % QUEUE_BYTES;
      data += run;
      length -= run;
      used_ += run;
    }
  }

  void read(uint8_t* out, size_t length) {
    while (length > 0) {
      const size_t run = std::min(length, QUEUE_BYTES - tail_);
      std::memcpy(out, buffer_ + tail_, run);
      tail_ = (tail_ + run) % QUEUE_BYTES;
      out += run;
      length -= run;
      used_ -= run;
    }
  }

  uint8_t* buffer_ = nullptr;
  SemaphoreHandle_t lock_ = nullptr;
  size_t head_ = 0;
  size_t tail_ = 0;
  std::atomic<size_t> used_{0};
  std::atomic<uint32_t> generation_{0};
};

FeedQueue queue;

void senderTask(void*) {
  static Record record;
  for (;;) {
    if (!queue.pop(record)) {
      vTaskDelay(pdMS_TO_TICKS(5));
      continue;
    }
    while (record.generation == queue.generation()) {
      bool sent = false;
      switch (record.header.kind) {
        case RecordKind::Start: {
          audiolink::StreamStartPayload start{};
          start.streamId = record.header.streamId;
          start.codec = record.data[0];
          std::memcpy(&start.startMs, record.data + 1, sizeof(start.startMs));
          sent = AudioLink::send(Type::StreamStart, &start, sizeof(start), 200);
          break;
        }
        case RecordKind::Data:
          sent = AudioLink::sendAudio(record.header.streamId, record.data, record.header.length, 200);
          break;
        case RecordKind::End:
          sent = AudioLink::send(Type::StreamEnd, &record.header.streamId, 1, 200);
          break;
      }
      if (sent) break;
      // Module away: hold the record. A new session restarts the stream, which clears the queue.
      if (!AudioLink::ready()) vTaskDelay(pdMS_TO_TICKS(50));
    }
  }
}

// ---------------------------------------------------------------------------------------------
// Network feeder: streams Audiobookshelf tracks over HTTP into the queue. It never touches the SD
// card, so unlike the SD path it can run on its own task.

struct FeedJob {
  std::string url;
  std::string name;
  uint32_t startMs = 0;
  uint8_t streamId = 0;
  uint32_t generation = 0;
};

enum class FeedState : uint8_t { Idle, Running, Ended, Failed };

SemaphoreHandle_t feedLock = nullptr;  // pendingJob, jobPending, activeFile, feedError
TaskHandle_t feedTask = nullptr;
FeedJob pendingJob;
bool jobPending = false;
HttpAudioFile* activeFile = nullptr;
std::string feedError;
std::atomic<uint8_t> feedStreamId{0};
std::atomic<FeedState> feedState{FeedState::Idle};
std::atomic<uint32_t> feedStartMs{0};
std::atomic<bool> feedStartKnown{false};

class FeedGuard {
 public:
  FeedGuard() { xSemaphoreTake(feedLock, portMAX_DELAY); }
  ~FeedGuard() { xSemaphoreGive(feedLock); }
  FeedGuard(const FeedGuard&) = delete;
  FeedGuard& operator=(const FeedGuard&) = delete;
};

bool jobStale(const FeedJob& job) {
  if (queue.generation() != job.generation) return true;
  FeedGuard guard;
  return jobPending;
}

void setActiveFile(HttpAudioFile* file) {
  FeedGuard guard;
  activeFile = file;
}

std::atomic<uint32_t> feedBytes{0};  // bytes the feeder queued, all streams (stream health log)

// The last job's source and file (feeder task only), reused when the next job is the same track.
std::unique_ptr<AudioSource> keptSource;
HttpAudioFile* keptFile = nullptr;  // owned by keptSource
std::string keptUrl;

void runFeedJob(const FeedJob& job, bool& holdsWifi) {
  feedStreamId = job.streamId;
  feedStartKnown = false;
  feedState = FeedState::Running;
  auto fail = [](const std::string& why) {
    {
      FeedGuard guard;
      feedError = why;
    }
    feedState = FeedState::Failed;
  };

  std::string error;
  const uint32_t t0 = millis();
  if (holdsWifi && !AbsWifi::connected()) {
    // The network dropped under the hold: give it back so acquire() joins again.
    AbsWifi::release();
    holdsWifi = false;
  }
  if (!holdsWifi) {
    if (!AbsWifi::acquire(error)) return fail(error);
    holdsWifi = true;
  }
  const uint32_t tWifi = millis();
  // The same track again (a skip, a seek): reuse the parsed source and its cached blocks, rather
  // than read the header and the MP4 tables over new connections, which took 15-20 s.
  std::unique_ptr<AudioSource> trackSource;
  HttpAudioFile* raw = nullptr;
  const bool reused = keptSource && keptUrl == job.url;
  if (reused) {
    trackSource = std::move(keptSource);
    raw = keptFile;
    raw->resume();
    setActiveFile(raw);
  } else {
    keptSource.reset();
    keptFile = nullptr;
    auto file = std::make_unique<HttpAudioFile>(Abs::url(job.url));
    raw = file.get();
    setActiveFile(raw);
    if (!raw->open(error)) {
      setActiveFile(nullptr);
      return fail(error);
    }
    trackSource = AudioSource::forName(job.name);
    if (!trackSource || !trackSource->open(std::move(file))) {
      setActiveFile(nullptr);
      return fail(trackSource ? trackSource->error() : "unsupported file type");
    }
  }
  const uint32_t tOpen = millis();
  // Kept for the next job unless a read failed on the network (not merely cancelled).
  auto keep = [&] {
    if (raw->failed() && !raw->cancelled()) return;
    raw->disconnect();
    keptSource = std::move(trackSource);
    keptFile = raw;
    keptUrl = job.url;
  };
  const uint32_t actual = trackSource->seek(job.startMs);
  if (jobStale(job)) {
    setActiveFile(nullptr);
    keep();
    return;
  }
  // Where a stream's start goes; the module starts playing once its first audio arrives.
  AudioModule::logf("Feed: at %lu s; Wi-Fi %lu ms, %s %lu ms, seek %lu ms", static_cast<unsigned long>(actual / 1000),
                    static_cast<unsigned long>(tWifi - t0), reused ? "same file" : "open + header",
                    static_cast<unsigned long>(tOpen - tWifi), static_cast<unsigned long>(millis() - tOpen));
  feedStartMs = actual;
  feedStartKnown = true;
  uint8_t start[5];
  start[0] = static_cast<uint8_t>(trackSource->codec());
  std::memcpy(start + 1, &actual, sizeof(actual));
  queue.push(RecordKind::Start, job.streamId, start, sizeof(start), job.generation);

  static uint8_t chunk[DATA_CHUNK];  // this task only
  for (;;) {
    if (jobStale(job)) break;
    if (queue.freeBytes() < sizeof(RecordHeader) + DATA_CHUNK + 16) {
      vTaskDelay(pdMS_TO_TICKS(20));
      continue;
    }
    const size_t n = trackSource->read(chunk, sizeof(chunk));
    if (n == 0) {
      if (raw->failed()) {
        fail(raw->lastError());
      } else {
        queue.push(RecordKind::End, job.streamId, nullptr, 0, job.generation);
        feedState = FeedState::Ended;
      }
      break;
    }
    queue.push(RecordKind::Data, job.streamId, chunk, static_cast<uint16_t>(n), job.generation);
    feedBytes += n;
  }
  setActiveFile(nullptr);
  keep();
}

void feedTaskMain(void*) {
  bool holdsWifi = false;  // across jobs, until FEED_WIFI_LINGER_MS pass without one
  for (;;) {
    const uint32_t woken = ulTaskNotifyTake(pdTRUE, holdsWifi ? pdMS_TO_TICKS(FEED_WIFI_LINGER_MS) : portMAX_DELAY);
    if (woken == 0 && holdsWifi) {
      AbsWifi::release();
      holdsWifi = false;
      continue;
    }
    for (;;) {
      FeedJob job;
      {
        FeedGuard guard;
        if (!jobPending) break;
        job = pendingJob;
        jobPending = false;
      }
      runFeedJob(job, holdsWifi);
    }
  }
}

void postFeedJob(const FeedJob& job) {
  {
    FeedGuard guard;
    pendingJob = job;
    jobPending = true;
    if (activeFile) activeFile->cancel();
  }
  xTaskNotifyGive(feedTask);
}

void cancelFeed() {
  FeedGuard guard;
  jobPending = false;
  if (activeFile) activeFile->cancel();
}

// ---------------------------------------------------------------------------------------------
// Progress sync with Audiobookshelf, on its own task (network only); results go back to the
// main loop through syncResult.

struct SyncRequest {
  bool push = false;
  bool onlyIfConnected = false;  ///< skip it when Wi-Fi is off rather than turn it on
  std::string itemId;
  uint32_t bookMs = 0;
  uint32_t durationMs = 0;
  bool finished = false;
  uint64_t syncedUpdateMs = 0;  ///< a push: the server's lastUpdate at our last sync
};

struct SyncResult {
  bool valid = false;
  bool push = false;
  bool ok = false;
  std::string itemId;
  std::string error;
  Abs::Progress progress;
  uint32_t pushedBookMs = 0;
};

SemaphoreHandle_t syncLock = nullptr;
TaskHandle_t syncTask = nullptr;
// One pending push and one pending fetch (a newer one replaces its kind): a fetch queued behind
// the closing push of the previous book must not be lost, nor a push behind a fetch. Pushes go
// first. Results queue up for the main loop, so none overwrites another before it is seen.
SyncRequest pendingPush;
SyncRequest pendingFetch;
bool pushPending = false;
bool fetchPending = false;
std::deque<SyncResult> syncResults;

void syncTaskMain(void*) {
  // The lastUpdate our own last push left (sync task only), so a push queued behind it is not
  // mistaken for another device's change.
  std::string ownPushItemId;
  uint64_t ownPushUpdateMs = 0;
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    for (;;) {
      SyncRequest request;
      xSemaphoreTake(syncLock, portMAX_DELAY);
      const bool any = pushPending || fetchPending;
      if (pushPending) {
        request = std::move(pendingPush);
        pushPending = false;
      } else if (fetchPending) {
        request = std::move(pendingFetch);
        fetchPending = false;
      }
      xSemaphoreGive(syncLock);
      if (!any) break;

      SyncResult result;
      result.valid = true;
      result.push = request.push;
      result.itemId = request.itemId;
      result.pushedBookMs = request.bookMs;
      if (request.onlyIfConnected && !AbsWifi::connected()) continue;
      AbsWifi::Use wifi(result.error);
      if (wifi.ok()) {
        const Abs::KeepConnection keep;  // the check, the save and the read-back on one connection
        result.ok = Abs::progress(request.itemId, result.progress, result.error);
        // A push never overwrites a position another device moved on to since our last sync (or
        // our own last push): it may have been queued before the main loop saw that position.
        // Reported as a fetch instead, so applySyncResult() adopts it.
        const Abs::Progress& server = result.progress;
        const bool movedElsewhere = server.exists && server.lastUpdateMs != request.syncedUpdateMs &&
                                    !(request.itemId == ownPushItemId && server.lastUpdateMs == ownPushUpdateMs) &&
                                    server.currentMs > request.bookMs + SYNC_TOLERANCE_MS;
        if (result.ok && request.push && movedElsewhere) {
          result.push = false;
          AudioModule::logf("Sync: not pushing %lu s, the server is at %lu s",
                            static_cast<unsigned long>(request.bookMs / 1000),
                            static_cast<unsigned long>(server.currentMs / 1000));
        } else if (result.ok && request.push) {
          result.ok =
              Abs::updateProgress(request.itemId, request.bookMs, request.durationMs, request.finished, result.error);
          // Read it back: its lastUpdate is what "in sync" means from now on.
          if (result.ok) result.ok = Abs::progress(request.itemId, result.progress, result.error);
          if (result.ok) {
            ownPushItemId = request.itemId;
            ownPushUpdateMs = result.progress.lastUpdateMs;
          }
        }
      }
      xSemaphoreTake(syncLock, portMAX_DELAY);
      syncResults.push_back(std::move(result));
      while (syncResults.size() > 4) syncResults.pop_front();
      xSemaphoreGive(syncLock);
    }
  }
}

void postSync(const SyncRequest& request) {
  if (!syncTask) return;
  xSemaphoreTake(syncLock, portMAX_DELAY);
  if (request.push) {
    pendingPush = request;
    pushPending = true;
  } else {
    pendingFetch = request;
    fetchPending = true;
  }
  xSemaphoreGive(syncLock);
  xTaskNotifyGive(syncTask);
}

SyncResult takeSyncResult() {
  SyncResult result;
  xSemaphoreTake(syncLock, portMAX_DELAY);
  if (!syncResults.empty()) {
    result = std::move(syncResults.front());
    syncResults.pop_front();
  }
  xSemaphoreGive(syncLock);
  return result;
}

// ---------------------------------------------------------------------------------------------
// Player state (main loop only).

struct TrackInfo {
  std::string path;  ///< on the SD card, empty when streamed
  std::string url;   ///< server-relative, streamed tracks only
  std::string name;  ///< file name, picks the reader
  uint32_t startOffsetMs = 0;
  uint32_t durationMs = 0;
};

struct Progress {
  int track = 0;
  uint32_t ms = 0;
  bool dirty = false;           ///< listened since the last successful sync
  uint64_t syncedUpdateMs = 0;  ///< the server's lastUpdate at the last sync
};

bool isOpen = false;
bool paused = false;
bool finished = false;
bool remote = false;
Book book;
std::string progressKey;
std::vector<TrackInfo> tracks;
std::vector<AudioChapter> bookChapters;  // in the whole book; empty when the book has none
std::string coverPath;                   // see View::coverPath
uint32_t pausedAtMs = 0;                 // when the current pause began
uint32_t handedWifiAtMs = 0;             // resumeAfterScreen() holds an AbsWifi user since then (0: none)
bool wifiParked = false;                 // streamed book paused long enough to stop the stream
std::string bookAuthor;                  // streamed books: for the recent list
int track = 0;
std::unique_ptr<AudioSource> source;  // SD tracks only; streamed tracks live on the feeder task
uint32_t trackDurationMs = 0;

uint8_t streamId = 0;
bool streamActive = false;
bool endQueued = false;
uint32_t streamStartMs = 0;
uint32_t positionMs = 0;
uint32_t session = 0;
uint32_t lastSaveMs = 0;
uint32_t lastModuleTryMs = 0;
uint32_t retryStreamAtMs = 0;
std::string message;

bool syncDirty = false;
uint64_t syncedUpdateMs = 0;
uint32_t lastPushedBookMs = 0;
uint32_t lastPushMs = 0;

std::string baseName(const std::string& path) {
  const size_t slash = path.find_last_of('/');
  std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
  const size_t dot = name.find_last_of('.');
  if (dot != std::string::npos && dot > 0) name.erase(dot);
  return name;
}

/** "Part 2" before "Part 10". */
bool naturalLess(const std::string& a, const std::string& b) {
  size_t i = 0, j = 0;
  while (i < a.size() && j < b.size()) {
    if (std::isdigit(static_cast<unsigned char>(a[i])) && std::isdigit(static_cast<unsigned char>(b[j]))) {
      size_t ei = i, ej = j;
      while (ei < a.size() && std::isdigit(static_cast<unsigned char>(a[ei]))) ++ei;
      while (ej < b.size() && std::isdigit(static_cast<unsigned char>(b[ej]))) ++ej;
      const std::string na = a.substr(i, ei - i), nb = b.substr(j, ej - j);
      const size_t za = na.find_first_not_of('0'), zb = nb.find_first_not_of('0');
      const std::string ta = za == std::string::npos ? "0" : na.substr(za);
      const std::string tb = zb == std::string::npos ? "0" : nb.substr(zb);
      if (ta.size() != tb.size()) return ta.size() < tb.size();
      if (ta != tb) return ta < tb;
      i = ei;
      j = ej;
      continue;
    }
    const int ca = std::tolower(static_cast<unsigned char>(a[i]));
    const int cb = std::tolower(static_cast<unsigned char>(b[j]));
    if (ca != cb) return ca < cb;
    ++i;
    ++j;
  }
  // Equal so far: the one that ran out first sorts first.
  return i == a.size() && j < b.size();
}

constexpr const char* LOCAL_COVERS = "/.system/cache/audio-covers";
constexpr const char* RECENT_FILE = "/.system/audio_recent.json";
constexpr size_t RECENT_MAX = 8;

std::vector<RecentStream> loadRecent() {
  std::vector<RecentStream> out;
  SdIoMutex::Lock lock;
  FsFile file = SdMan.open(RECENT_FILE, O_RDONLY);
  if (!file) return out;
  std::string text;
  char chunk[256];
  for (int n; (n = file.read(chunk, sizeof(chunk))) > 0;) text.append(chunk, static_cast<size_t>(n));
  file.close();
  JsonDocument doc;
  if (deserializeJson(doc, text) != DeserializationError::Ok) return out;
  for (JsonObject r : doc.as<JsonArray>()) {
    RecentStream entry;
    entry.itemId = r["id"] | "";
    entry.title = r["title"] | "";
    entry.author = r["author"] | "";
    entry.positionMs = r["positionMs"] | 0u;
    entry.durationMs = r["durationMs"] | 0u;
    if (!entry.itemId.empty()) out.push_back(entry);
  }
  return out;
}

void saveRecent(const std::vector<RecentStream>& recent) {
  JsonDocument doc;
  for (const RecentStream& r : recent) {
    JsonObject o = doc.add<JsonObject>();
    o["id"] = r.itemId;
    o["title"] = r.title;
    o["author"] = r.author;
    o["positionMs"] = r.positionMs;
    o["durationMs"] = r.durationMs;
  }
  std::string text;
  serializeJson(doc, text);
  SdIoMutex::Lock lock;
  SdMan.mkdir("/.system");
  FsFile file = SdMan.open(RECENT_FILE, O_WRONLY | O_CREAT | O_TRUNC);
  if (!file) return;
  file.write(text.data(), text.size());
  file.close();
}

/** The streamed book to the front of the recent list, with its position now. */
void noteRecent(const std::string& itemId, const std::string& title, const std::string& author,
                const uint32_t positionMs, const uint32_t durationMs) {
  std::vector<RecentStream> recent = loadRecent();
  recent.erase(
      std::remove_if(recent.begin(), recent.end(), [&itemId](const RecentStream& r) { return r.itemId == itemId; }),
      recent.end());
  recent.insert(recent.begin(), RecentStream{itemId, title, author, positionMs, durationMs});
  if (recent.size() > RECENT_MAX) recent.resize(RECENT_MAX);
  saveRecent(recent);
}

bool isImageName(const std::string& name, const bool preferredOnly) {
  std::string lower = name;
  std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });
  const size_t dot = lower.rfind('.');
  if (dot == std::string::npos || lower[0] == '.') return false;
  const std::string ext = lower.substr(dot + 1);
  if (ext != "jpg" && ext != "jpeg" && ext != "png") return false;
  const std::string stem = lower.substr(0, dot);
  return !preferredOnly || stem == "cover" || stem == "folder" || stem == "front";
}

/** A folder book's cover: cover/folder/front.jpg|png, else its first image. Holds SdIoMutex. */
std::string folderCover(const std::string& folder) {
  SdIoMutex::Lock lock;
  FsFile dir = SdMan.open(folder.c_str(), O_RDONLY);
  if (!dir || !dir.isDirectory()) return "";
  std::string first;
  std::string preferred;
  char name[128];
  for (FsFile entry = dir.openNextFile(); entry; entry = dir.openNextFile()) {
    entry.getName(name, sizeof(name));
    const bool file = !entry.isDirectory();
    entry.close();
    if (!file || !isImageName(name, false)) continue;
    if (first.empty()) first = name;
    if (isImageName(name, true)) {
      preferred = name;
      break;
    }
  }
  dir.close();
  const std::string chosen = preferred.empty() ? first : preferred;
  return chosen.empty() ? "" : folder + "/" + chosen;
}

/**
 * Copies cover art embedded in an audio file into the cache once (named by a hash of the file's
 * path), so the screen draws a small file rather than seeking into a large one. Holds SdIoMutex.
 */
/** Where an audio file's embedded cover is cached (named by a hash of the file's path). */
std::string embeddedCoverPath(const std::string& audioPath, const bool png) {
  uint32_t hash = 2166136261u;
  for (const unsigned char c : audioPath) hash = (hash ^ c) * 16777619u;
  char name[48];
  snprintf(name, sizeof(name), "%s/%08x.%s", LOCAL_COVERS, static_cast<unsigned>(hash), png ? "png" : "jpg");
  return name;
}

std::string cacheEmbeddedCover(const std::string& audioPath, const uint64_t offset, const uint32_t length,
                               const bool png) {
  const std::string cached = embeddedCoverPath(audioPath, png);
  const char* name = cached.c_str();
  SdIoMutex::Lock lock;
  if (SdMan.exists(name)) return name;
  SdMan.mkdir("/.system/cache");
  SdMan.mkdir(LOCAL_COVERS);
  FsFile in = SdMan.open(audioPath.c_str(), O_RDONLY);
  const std::string temporary = std::string(name) + ".tmp";
  FsFile out = SdMan.open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC);
  bool ok = in && out && in.seekSet(offset);
  uint8_t buffer[2048];
  for (uint32_t left = length; ok && left > 0;) {
    const int n = in.read(buffer, std::min<uint32_t>(left, sizeof(buffer)));
    ok = n > 0 && out.write(buffer, static_cast<size_t>(n)) == static_cast<size_t>(n);
    left -= n > 0 ? static_cast<uint32_t>(n) : left;
  }
  if (in) in.close();
  if (out) out.close();
  if (ok) ok = SdMan.rename(temporary.c_str(), name);
  if (!ok) SdMan.remove(temporary.c_str());
  return ok ? name : "";
}

std::vector<std::string> listAudioFiles(const std::string& folder) {
  std::vector<std::string> files;
  FsFile dir = SdMan.open(folder.c_str(), O_RDONLY);
  if (!dir || !dir.isDirectory()) return files;
  char name[256];
  for (;;) {
    FsFile entry = dir.openNextFile();
    if (!entry) break;
    entry.getName(name, sizeof(name));
    const bool isFile = !entry.isDirectory();
    entry.close();
    if (isFile && name[0] != '.' && AudioSource::isAudioFile(name)) files.push_back(folder + "/" + name);
  }
  dir.close();
  std::sort(files.begin(), files.end(), naturalLess);
  return files;
}

bool isAbsBook() { return !book.absItemId.empty(); }

uint32_t bookMs() {
  return track < static_cast<int>(tracks.size()) ? tracks[track].startOffsetMs + positionMs : positionMs;
}

uint32_t bookDurationMs() {
  if (tracks.empty()) return 0;
  return tracks.back().startOffsetMs + tracks.back().durationMs;
}

// Progress file, one line per book: "<track>\t<ms>\t<dirty>\t<synced>\t<key>". Lines from the first
// version have only "<track>\t<ms>\t<key>".
std::vector<std::pair<std::string, Progress>> loadAllProgress() {
  std::vector<std::pair<std::string, Progress>> entries;
  FsFile file = SdMan.open(PROGRESS_FILE, O_RDONLY);
  if (!file) return entries;
  std::string line;
  auto flush = [&]() {
    std::vector<std::string> fields;
    size_t at = 0;
    for (int i = 0; i < 4; ++i) {
      const size_t tab = line.find('\t', at);
      if (tab == std::string::npos) break;
      fields.push_back(line.substr(at, tab - at));
      at = tab + 1;
    }
    if (fields.size() >= 2) {
      Progress p;
      p.track = std::atoi(fields[0].c_str());
      p.ms = static_cast<uint32_t>(std::strtoul(fields[1].c_str(), nullptr, 10));
      std::string key;
      if (fields.size() == 4) {
        p.dirty = fields[2] == "1";
        p.syncedUpdateMs = std::strtoull(fields[3].c_str(), nullptr, 10);
        key = line.substr(at);
      } else {
        // Old line: everything after the second tab is the path.
        key = line.substr(fields[0].size() + fields[1].size() + 2);
      }
      entries.emplace_back(key, p);
    }
    line.clear();
  };
  char chunk[256];
  int got;
  while ((got = file.read(chunk, sizeof(chunk))) > 0) {
    for (int i = 0; i < got; ++i) {
      if (chunk[i] == '\n') {
        flush();
      } else if (chunk[i] != '\r') {
        line += chunk[i];
      }
    }
  }
  flush();
  file.close();
  return entries;
}

void saveProgress() {
  if (!isOpen) return;
  // A streamed book keeps its place on the Audiobooks page's recent list.
  if (remote) noteRecent(book.absItemId, book.title, bookAuthor, bookMs(), bookDurationMs());
  if (isAbsBook()) {
    const uint32_t now = bookMs();
    const uint32_t moved = now > lastPushedBookMs ? now - lastPushedBookMs : lastPushedBookMs - now;
    if (moved > SYNC_TOLERANCE_MS) syncDirty = true;
  }
  SdIoMutex::Lock lock;
  auto entries = loadAllProgress();
  const Progress now{track, positionMs, syncDirty, syncedUpdateMs};
  bool found = false;
  for (auto& entry : entries) {
    if (entry.first == progressKey) {
      entry.second = now;
      found = true;
    }
  }
  if (!found) entries.emplace_back(progressKey, now);
  SdMan.mkdir("/.system");
  FsFile file = SdMan.open(PROGRESS_FILE, O_WRONLY | O_CREAT | O_TRUNC);
  if (!file) return;
  for (const auto& entry : entries) {
    char prefix[64];
    const int n = snprintf(prefix, sizeof(prefix), "%d\t%lu\t%d\t%llu\t", entry.second.track,
                           static_cast<unsigned long>(entry.second.ms), entry.second.dirty ? 1 : 0,
                           static_cast<unsigned long long>(entry.second.syncedUpdateMs));
    file.write(prefix, n);
    file.write(entry.first.data(), entry.first.size());
    file.write("\n", 1);
  }
  file.close();
  lastSaveMs = millis();
}

void requestPush(const bool onlyIfConnected = false) {
  if (!isAbsBook() || !Abs::signedIn()) return;
  SyncRequest request;
  request.push = true;
  request.onlyIfConnected = onlyIfConnected;
  request.itemId = book.absItemId;
  request.bookMs = finished ? bookDurationMs() : bookMs();
  request.durationMs = bookDurationMs();
  request.finished = finished;
  request.syncedUpdateMs = syncedUpdateMs;
  postSync(request);
  lastPushMs = millis();
}

/** Reads the server's position (adopted by applySyncResult() if it moved on elsewhere). */
void requestFetch() {
  if (!isAbsBook() || !Abs::signedIn()) return;
  SyncRequest fetch;
  fetch.itemId = book.absItemId;
  postSync(fetch);
}

void sendControl(const Type type, const void* payload = nullptr, const size_t length = 0) {
  if (AudioLink::ready()) AudioLink::send(type, payload, length, CONTROL_TIMEOUT_MS);
}

void stopStream() {
  queue.clear();
  if (remote) cancelFeed();
  if (streamActive) sendControl(Type::StreamStop, &streamId, 1);
  streamActive = false;
  endQueued = false;
  retryStreamAtMs = 0;
}

bool openTrack(const int index) {
  source.reset();
  if (index < 0 || index >= static_cast<int>(tracks.size())) return false;
  track = index;
  if (remote) {
    // The feeder opens streamed tracks; the duration comes from the server.
    trackDurationMs = tracks[index].durationMs;
    return true;
  }
  const TrackInfo& info = tracks[index];
  auto next = AudioSource::forName(info.name);
  if (!next) {
    message = "Unsupported file: " + baseName(info.name);
    return false;
  }
  SdIoMutex::Lock lock;
  if (!next->open(AudioFile::openSd(info.path))) {
    message = baseName(info.name) + ": " + next->error();
    AudioModule::logf("Player: %s", message.c_str());
    return false;
  }
  trackDurationMs = next->durationMs() ? next->durationMs() : info.durationMs;
  // A local book's offsets and durations are known only once its files are opened.
  if (tracks[index].durationMs == 0) tracks[index].durationMs = trackDurationMs;
  // A single .m4b has its chapters inside: used unless the server's are known (downloads made
  // before chapters were saved have none, and the file's are the same ones).
  if (tracks.size() == 1 && bookChapters.empty()) bookChapters = next->chapters();
  // Cover art in the file, when the book has no other cover yet.
  uint64_t coverOffset = 0;
  uint32_t coverLength = 0;
  bool coverPng = false;
  if (coverPath.empty() && next->embeddedCover(coverOffset, coverLength, coverPng)) {
    coverPath = cacheEmbeddedCover(info.path, coverOffset, coverLength, coverPng);
  }
  source = std::move(next);
  return true;
}

/** Starts a new stream at ms in the current track. The module drops anything of the old one. */
void startStream(uint32_t ms) {
  stopStream();
  if (!remote && !source) return;
  if (wifiParked) {
    // Any restart (play, a seek, a chapter) ends the Wi-Fi pause; only playing waits on Wi-Fi.
    wifiParked = false;
    message = paused ? "" : WIFI_RECONNECTING_MESSAGE;
  }
  if (paused) pausedAtMs = millis();  // a seek while paused starts the wait again
  if (trackDurationMs && ms >= trackDurationMs) ms = trackDurationMs > 1000 ? trackDurationMs - 1000 : 0;
  streamId = static_cast<uint8_t>(streamId + 1);
  finished = false;
  if (remote) {
    AudioModule::logf("Player: stream from %lu s", static_cast<unsigned long>(ms / 1000));
    FeedJob job;
    job.url = tracks[track].url;
    job.name = tracks[track].name;
    job.startMs = ms;
    job.streamId = streamId;
    job.generation = queue.generation();
    postFeedJob(job);
  } else {
    {
      SdIoMutex::Lock lock;
      ms = source->seek(ms);
    }
    uint8_t start[5];
    start[0] = static_cast<uint8_t>(source->codec());
    std::memcpy(start + 1, &ms, sizeof(ms));
    queue.push(RecordKind::Start, streamId, start, sizeof(start));
  }
  streamStartMs = ms;
  positionMs = ms;
  streamActive = true;
  sendControl(paused ? Type::Pause : Type::Resume);
}

/** The chapter playing at ms in the book, -1 before the first (or without chapters). */
int chapterAt(const uint32_t ms) {
  int found = -1;
  for (int i = 0; i < static_cast<int>(bookChapters.size()); ++i) {
    if (bookChapters[i].startMs <= ms) found = i;
  }
  return found;
}

/** Where chapter c ends: the next one's start, or the end of the book (0 if unknown). */
uint32_t chapterEndMs(const int c) {
  if (c + 1 < static_cast<int>(bookChapters.size())) return bookChapters[c + 1].startMs;
  return bookDurationMs();
}

/** Jumps to a position in the whole book (tracks with offsets: Audiobookshelf books, single files). */
void seekBook(const uint32_t ms) {
  int target = 0;
  for (int i = 0; i < static_cast<int>(tracks.size()); ++i) {
    if (tracks[i].startOffsetMs <= ms) target = i;
  }
  if (target != track && !openTrack(target)) return;
  startStream(ms - tracks[target].startOffsetMs);
}

}  // namespace

void wakeModule() {
  if (millis() - lastModuleTryMs < MODULE_RETRY_MS && lastModuleTryMs != 0) return;
  lastModuleTryMs = millis();
  // Link first, then power: the bridge enumerates once VBUS comes up (see AudioModule).
  if (!AudioModule::isLinkEnabled()) AudioModule::setLinkEnabled(true);
  if (!AudioModule::isPowerOn()) {
    const auto snapshot = AudioModule::snapshot();
    if (!snapshot.externalVbus) AudioModule::setPower(true);
  }
}

namespace {

void refill() {
  if (remote || !streamActive || endQueued || !source) return;
  static uint8_t chunk[DATA_CHUNK];
  SdIoMutex::Lock lock;
  for (int i = 0; i < REFILL_RECORDS_PER_SERVICE; ++i) {
    if (queue.freeBytes() < sizeof(RecordHeader) + DATA_CHUNK + 16) return;
    const size_t n = source->read(chunk, sizeof(chunk));
    if (n == 0) {
      queue.push(RecordKind::End, streamId, nullptr, 0);
      endQueued = true;
      return;
    }
    queue.push(RecordKind::Data, streamId, chunk, static_cast<uint16_t>(n));
  }
}

/** Picks up what the network feeder did for the current stream. */
void updateFromFeeder() {
  if (!remote || !streamActive || feedStreamId != streamId) return;
  if (feedStartKnown) {
    feedStartKnown = false;
    streamStartMs = feedStartMs;
  }
  const FeedState state = feedState;
  if (state == FeedState::Ended) endQueued = true;
  if (state == FeedState::Failed && retryStreamAtMs == 0) {
    {
      FeedGuard guard;
      message = "Network: " + feedError + ", retrying";
    }
    AudioModule::logf("Player: %s", message.c_str());
    retryStreamAtMs = millis() + STREAM_RETRY_MS;
  }
}

void finishTrack() {
  if (track + 1 < static_cast<int>(tracks.size()) && openTrack(track + 1)) {
    startStream(0);
    saveProgress();
    requestPush();
    return;
  }
  streamActive = false;
  finished = true;
  positionMs = trackDurationMs;
  saveProgress();
  requestPush();
}

void updateFromModule() {
  const AudioLink::ModuleStatus module = AudioLink::status();
  if (!module.valid || !streamActive || module.status.streamId != streamId) return;
  const auto state = static_cast<PlayState>(module.status.playState);
  if (state == PlayState::Idle) return;
  positionMs = streamStartMs + module.status.playedMs;
  if (trackDurationMs && positionMs > trackDurationMs) positionMs = trackDurationMs;
  if (state == PlayState::Playing && message == WIFI_RECONNECTING_MESSAGE) message.clear();  // back
  if (state == PlayState::Finished && endQueued) finishTrack();
}

/**
 * Reconciles with the server, without trusting either clock: the server changed if its lastUpdate
 * moved since our last sync, and we changed if we listened since then (syncDirty). Only the server
 * changed: adopt it. Only we changed: push. Both: the further position wins.
 */
void applySyncResult() {
  const SyncResult result = takeSyncResult();
  if (!result.valid || !isOpen || result.itemId != book.absItemId) return;
  if (!result.push) {
    AudioModule::logf(
        "Sync: %s, server %s at %lu s, here %lu s%s", result.ok ? "fetched" : "fetch failed",
        result.progress.exists ? (result.progress.lastUpdateMs != syncedUpdateMs ? "changed" : "same") : "none",
        static_cast<unsigned long>(result.progress.currentMs / 1000), static_cast<unsigned long>(bookMs() / 1000),
        syncDirty ? " (listened)" : "");
  }
  if (!result.ok) {
    AudioModule::logf("Sync: %s", result.error.c_str());
    return;
  }
  const Abs::Progress& server = result.progress;
  if (result.push) {
    syncedUpdateMs = server.lastUpdateMs;
    lastPushedBookMs = result.pushedBookMs;
    const uint32_t now = bookMs();
    const uint32_t moved = now > lastPushedBookMs ? now - lastPushedBookMs : lastPushedBookMs - now;
    syncDirty = moved > SYNC_TOLERANCE_MS;
    saveProgress();
    return;
  }
  if (!server.exists) {
    if (bookMs() > 0) requestPush();
    return;
  }
  const bool serverChanged = server.lastUpdateMs != syncedUpdateMs;
  if (!serverChanged) {
    if (syncDirty) requestPush();
    return;
  }
  if (!syncDirty || server.currentMs > bookMs()) {
    const uint32_t local = bookMs();
    const uint32_t gap = server.currentMs > local ? server.currentMs - local : local - server.currentMs;
    syncedUpdateMs = server.lastUpdateMs;
    lastPushedBookMs = server.currentMs;
    syncDirty = false;
    if (gap > SYNC_TOLERANCE_MS) {
      seekBook(server.currentMs);
      message = "Position synced from Audiobookshelf";
      AudioModule::logf("Sync: moved to %lu s from the server", static_cast<unsigned long>(server.currentMs / 1000));
    }
    saveProgress();
  } else {
    requestPush();
  }
}

/** Common tail of open() and openRemote(): saved position, playback, first sync. */
bool startBook() {
  Progress saved;
  {
    SdIoMutex::Lock lock;
    const auto entries = loadAllProgress();
    const auto match =
        std::find_if(entries.begin(), entries.end(), [](const auto& entry) { return entry.first == progressKey; });
    if (match != entries.end()) saved = match->second;
  }
  if (saved.track < 0 || saved.track >= static_cast<int>(tracks.size())) saved = {};
  if (!openTrack(saved.track)) return false;
  isOpen = true;
  paused = false;
  finished = false;
  positionMs = saved.ms;
  syncDirty = saved.dirty;
  syncedUpdateMs = saved.syncedUpdateMs;
  lastPushedBookMs = syncDirty ? 0 : bookMs();
  lastPushMs = millis();  // the first periodic push comes an interval after the opening fetch
  lastModuleTryMs = 0;
  wakeModule();
  session = AudioLink::sessionCounter();
  if (AudioLink::ready()) startStream(saved.ms);
  requestFetch();
  AudioModule::logf("Player: %s%s, track %d/%d at %lu s", book.title.c_str(), remote ? " (streaming)" : "", track + 1,
                    static_cast<int>(tracks.size()), static_cast<unsigned long>(saved.ms / 1000));
  return true;
}

}  // namespace

void begin() {
  if (!queue.begin()) {
    AudioModule::logf("Player: out of memory");
    return;
  }
  feedLock = xSemaphoreCreateMutex();
  syncLock = xSemaphoreCreateMutex();
  xTaskCreate(senderTask, "audio_send", 6144, nullptr, 3, nullptr);
  // TLS needs a deep stack.
  xTaskCreate(feedTaskMain, "audio_feed", 12288, nullptr, 2, &feedTask);
  xTaskCreate(syncTaskMain, "abs_sync", 10240, nullptr, 1, &syncTask);
}

std::vector<Book> listBooks() {
  std::vector<Book> books;
  SdIoMutex::Lock lock;
  FsFile dir = SdMan.open(BOOKS_DIRECTORY, O_RDONLY);
  if (!dir || !dir.isDirectory()) return books;
  char name[256];
  for (;;) {
    FsFile entry = dir.openNextFile();
    if (!entry) break;
    entry.getName(name, sizeof(name));
    const bool isDirectory = entry.isDirectory();
    entry.close();
    if (name[0] == '.') continue;
    const std::string path = std::string(BOOKS_DIRECTORY) + "/" + name;
    if (isDirectory) {
      AbsLocal::BookMeta meta;
      const bool fromAbs = AbsLocal::read(path, meta);
      if (fromAbs && !meta.complete) continue;  // still downloading
      if (!listAudioFiles(path).empty()) {
        std::string title = fromAbs && !meta.title.empty() ? meta.title : name;
        // A podcast episode: its podcast first, so episodes sort together and say what they are.
        if (fromAbs && Abs::isEpisode(meta.itemId) && !meta.author.empty()) title = meta.author + " - " + title;
        Book found{path, title, true};
        if (fromAbs) found.absItemId = meta.itemId;
        books.push_back(found);
      }
    } else if (AudioSource::isAudioFile(name)) {
      books.push_back({path, baseName(name), false});
    }
  }
  dir.close();
  std::sort(books.begin(), books.end(), [](const Book& a, const Book& b) { return naturalLess(a.title, b.title); });
  return books;
}

std::vector<RecentStream> recentStreams() { return loadRecent(); }

std::string coverFor(const Book& b) {
  if (!b.absItemId.empty() && AbsCovers::cached(b.absItemId)) return AbsCovers::path(b.absItemId);
  if (b.folder) return folderCover(b.path);
  SdIoMutex::Lock lock;
  for (const bool png : {false, true}) {
    const std::string cached = embeddedCoverPath(b.path, png);
    if (SdMan.exists(cached.c_str())) return cached;
  }
  return "";
}

bool open(const Book& next) {
  stop();
  wifiParked = false;
  message.clear();
  book = next;
  remote = false;
  tracks.clear();
  bookChapters.clear();
  coverPath.clear();
  AbsLocal::BookMeta meta;
  if (next.folder && AbsLocal::read(next.path, meta) && meta.complete) {
    // Downloaded from Audiobookshelf: the server's track order and offsets, and progress sync.
    book.absItemId = meta.itemId;
    std::transform(meta.tracks.begin(), meta.tracks.end(), std::back_inserter(tracks), [&next](const Abs::Track& t) {
      return TrackInfo{next.path + "/" + t.filename, "", t.filename, t.startOffsetMs, t.durationMs};
    });
    bookChapters = meta.chapters;
  } else {
    book.absItemId.clear();
    std::vector<std::string> files;
    {
      SdIoMutex::Lock lock;
      files = next.folder ? listAudioFiles(next.path) : std::vector<std::string>{next.path};
    }
    std::transform(files.begin(), files.end(), std::back_inserter(tracks),
                   [](const std::string& file) { return TrackInfo{file, "", file, 0, 0}; });
  }
  if (tracks.empty()) {
    message = "No audio files";
    return false;
  }
  // The cover: the server's (cached, or fetched now if online) for an Audiobookshelf book, else an
  // image in the book's folder; a single file's embedded art is picked up when it opens.
  if (isAbsBook() && AbsCovers::cached(book.absItemId)) {
    coverPath = AbsCovers::path(book.absItemId);
  } else {
    if (isAbsBook()) AbsCovers::request({book.absItemId});
    if (next.folder) coverPath = folderCover(next.path);
  }
  // Downloaded and streamed copies of one Audiobookshelf book share their progress.
  progressKey = isAbsBook() ? ABS_KEY_PREFIX + book.absItemId : book.path;
  const bool started = startBook();  // opens the first track, which may have embedded art
  // Last resort for an Audiobookshelf book: the server's cover, shown once it has been fetched.
  if (coverPath.empty() && isAbsBook()) coverPath = AbsCovers::path(book.absItemId);
  return started;
}

bool openRemote(const Abs::ItemDetail& item) {
  stop();
  wifiParked = false;
  message.clear();
  book = Book{};
  book.title = item.title;
  book.absItemId = item.id;
  remote = true;
  tracks.clear();
  std::transform(item.tracks.begin(), item.tracks.end(), std::back_inserter(tracks), [](const Abs::Track& t) {
    return TrackInfo{"", t.contentUrl, t.filename, t.startOffsetMs, t.durationMs};
  });
  bookChapters = item.chapters;
  bookAuthor = item.author;
  // The server's cover: cached, or fetched now (the screen shows it once it is on the card).
  coverPath = AbsCovers::path(item.id);
  if (!AbsCovers::cached(item.id)) AbsCovers::request({item.id});
  if (tracks.empty()) {
    message = "No audio tracks";
    return false;
  }
  progressKey = ABS_KEY_PREFIX + item.id;
  const bool started = startBook();
  if (started) noteRecent(item.id, item.title, item.author, bookMs(), bookDurationMs());
  return started;
}

void stop() {
  if (!isOpen) return;
  saveProgress();
  requestPush();
  stopStream();
  source.reset();
  isOpen = false;
  remote = false;
}

void togglePause() {
  if (!isOpen) return;
  if (finished) {
    // Play again from the start of the book.
    if (openTrack(0)) startStream(0);
    paused = false;
    sendControl(Type::Resume);
    return;
  }
  paused = !paused;
  // Back after a while: the book may have been listened to on another device meanwhile.
  if (!paused && millis() - pausedAtMs >= RESUME_FETCH_AFTER_MS) requestFetch();
  if (!paused && wifiParked) {
    // The stream was stopped to let Wi-Fi go: start it again where it was.
    startStream(positionMs);
    return;
  }
  sendControl(paused ? Type::Pause : Type::Resume);
  if (paused) {
    pausedAtMs = millis();
    saveProgress();
    requestPush();
  }
}

bool suspendForScreen() {
  if (!isOpen || finished) return false;
  const bool wasPlaying = !paused;
  if (wasPlaying) togglePause();
  // Also a stream that is retrying (not active): no retries, and no Wi-Fi use, under the screen.
  if (remote && !wifiParked) {
    stopStream();
    wifiParked = true;
    message = WIFI_PARKED_MESSAGE;
  }
  AudioModule::logf("Player: suspended for a screen%s", wasPlaying ? " (was playing)" : "");
  return wasPlaying;
}

void resumeAfterScreen(const bool wasPlaying, const bool wifiHeld) {
  const bool resume = wasPlaying && isOpen && paused && !finished;
  if (resume) togglePause();
  if (wifiHeld) {
    if (resume && remote) {
      if (handedWifiAtMs != 0) AbsWifi::release();  // one hand-over at a time
      handedWifiAtMs = millis() | 1;
    } else {
      AbsWifi::release();
    }
  }
}

void skip(const int32_t deltaMs) {
  if (!isOpen || (!source && !remote)) return;
  const int64_t target = static_cast<int64_t>(positionMs) + deltaMs;
  if (target < 0 && deltaMs < 0 && track > 0 && positionMs < PREVIOUS_RESTARTS_TRACK_MS) {
    previousTrack();
    return;
  }
  if (trackDurationMs && target >= trackDurationMs && track + 1 < static_cast<int>(tracks.size())) {
    nextTrack();
    return;
  }
  startStream(static_cast<uint32_t>(std::max<int64_t>(0, target)));
}

void nextTrack() {
  if (!isOpen || track + 1 >= static_cast<int>(tracks.size())) return;
  if (openTrack(track + 1)) startStream(0);
  saveProgress();
  requestPush();
}

void previousTrack() {
  if (!isOpen) return;
  if (positionMs > PREVIOUS_RESTARTS_TRACK_MS || track == 0) {
    startStream(0);
    return;
  }
  if (openTrack(track - 1)) startStream(0);
  saveProgress();
  requestPush();
}

void seekChapter(const int index) {
  if (!isOpen || index < 0 || index >= static_cast<int>(bookChapters.size())) return;
  if (finished) {
    finished = false;
    paused = false;
    sendControl(Type::Resume);
  }
  seekBook(bookChapters[index].startMs);
  saveProgress();
  requestPush();
}

void nextChapter() {
  if (!isOpen || bookChapters.empty()) return;
  seekChapter(chapterAt(bookMs()) + 1);
}

void previousChapter() {
  if (!isOpen || bookChapters.empty()) return;
  const uint32_t now = bookMs();
  const int c = chapterAt(now);
  if (c < 0) {
    seekBook(0);
  } else if (now - bookChapters[c].startMs > PREVIOUS_RESTARTS_TRACK_MS || c == 0) {
    seekChapter(c);  // into the chapter: back to its start
  } else {
    seekChapter(c - 1);
  }
}

std::vector<AudioChapter> chapters() { return bookChapters; }

void setVolume(const uint8_t percent) {
  const uint8_t value = std::min<uint8_t>(percent, 100);
  sendControl(Type::SetVolume, &value, 1);
}

void selectOutput(const audiolink::Output output) {
  const uint8_t value = static_cast<uint8_t>(output);
  sendControl(Type::SetOutput, &value, 1);
}

void btScan(const bool start) {
  if (start) AudioLink::clearBtDevices();
  const uint8_t value = start ? 1 : 0;
  sendControl(Type::BtScan, &value, 1);
}

void btConnect(const uint8_t address[6]) { sendControl(Type::BtConnect, address, 6); }

void btForget() { sendControl(Type::BtForget); }

View view() {
  View v;
  v.open = isOpen;
  v.paused = paused;
  v.finished = finished;
  v.remote = remote;
  v.waitingForModule = isOpen && !AudioLink::ready();
  v.title = book.title;
  v.trackName = isOpen && track < static_cast<int>(tracks.size()) ? baseName(tracks[track].name) : "";
  v.track = track;
  v.trackCount = static_cast<int>(tracks.size());
  v.positionMs = positionMs;
  v.durationMs = trackDurationMs;
  v.bookPositionMs = isOpen ? bookMs() : 0;
  v.bookDurationMs = isOpen ? bookDurationMs() : 0;
  if (isOpen && !bookChapters.empty()) {
    const uint32_t now = bookMs();
    const int c = std::max(0, chapterAt(now));
    v.chapter = c;
    v.chapterCount = static_cast<int>(bookChapters.size());
    v.chapterTitle = bookChapters[c].title;
    v.chapterPositionMs = now > bookChapters[c].startMs ? now - bookChapters[c].startMs : 0;
    const uint32_t end = chapterEndMs(c);
    v.chapterDurationMs = end > bookChapters[c].startMs ? end - bookChapters[c].startMs : 0;
  }
  v.queuedBytes = static_cast<uint32_t>(queue.usedBytes());
  v.message = message;
  v.coverPath = isOpen ? coverPath : "";
  const AudioLink::ModuleStatus module = AudioLink::status();
  if (module.valid) {
    v.moduleState = static_cast<PlayState>(module.status.playState);
    v.btState = static_cast<audiolink::BtState>(module.status.btState);
    v.output = static_cast<audiolink::Output>(module.status.output);
    v.volume = module.status.volume;
  }
  return v;
}

bool keepAwake() { return isOpen && !paused && !finished; }

namespace {

/**
 * One log line on a playing stream every STREAM_HEALTH_LOG_MS: how fast it arrives, what is
 * buffered on each side, the module's underruns (what stutters), and how many connections and
 * single-block fetches it took (each a TLS handshake). Tells a slow network from a slow link.
 */
void logStreamHealth() {
  static uint32_t lastMs = 0;
  static uint32_t lastBytes = 0;
  static HttpAudioFile::Stats last{};
  static AudioLink::Counters lastLink{};
  const uint32_t now = millis();
  if (!remote || paused || !streamActive) {
    lastMs = 0;
    return;
  }
  const HttpAudioFile::Stats stats = HttpAudioFile::stats();
  const uint32_t bytes = feedBytes.load();
  const AudioLink::Counters link = AudioLink::counters();
  if (lastMs != 0 && now - lastMs >= STREAM_HEALTH_LOG_MS) {
    const AudioLink::ModuleStatus module = AudioLink::status();
    AudioModule::logf(
        "Stream: %lu KB/s in, queue %u KB; module %lu KB + %u ms, %u underruns, %u decode errors, %lu KB heap; "
        "%lu opens, %lu drops, %lu block fetches; internal %u KB block, %u KB free (min %u); "
        "link %lu resends, %lu write errors, %lu bad frames",
        static_cast<unsigned long>((bytes - lastBytes) / (now - lastMs)),
        static_cast<unsigned>(queue.usedBytes() / 1024),
        static_cast<unsigned long>(module.valid ? module.status.rxBuffered / 1024 : 0),
        module.valid ? module.status.pcmBufferedMs : 0, module.valid ? module.status.underruns : 0,
        module.valid ? module.status.decodeErrors : 0,
        static_cast<unsigned long>(module.valid ? module.status.freeHeap / 1024 : 0),
        static_cast<unsigned long>(stats.streamOpens - last.streamOpens),
        static_cast<unsigned long>(stats.streamDrops - last.streamDrops),
        static_cast<unsigned long>(stats.rangeFetches - last.rangeFetches),
        static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) / 1024),
        static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) / 1024),
        static_cast<unsigned>(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) / 1024),
        static_cast<unsigned long>(link.resends - lastLink.resends),
        static_cast<unsigned long>(link.writeErrors - lastLink.writeErrors),
        static_cast<unsigned long>(link.badFrames - lastLink.badFrames));
  } else if (lastMs != 0) {
    return;
  }
  lastMs = now;
  lastBytes = bytes;
  last = stats;
  lastLink = link;
}

}  // namespace

void service() {
  if (!isOpen) return;
  if (!paused && !finished) wakeModule();

  const uint32_t now = AudioLink::sessionCounter();
  if (now != session && AudioLink::ready()) {
    // New session: the module (re)connected or rebooted and knows nothing of our stream.
    session = now;
    if (!finished && !wifiParked) startStream(positionMs);
  }
  if (remote && paused && streamActive && !wifiParked && millis() - pausedAtMs >= PAUSED_WIFI_OFF_MS) {
    // Paused a while: stop the stream, which ends the feeder's hold on Wi-Fi once its linger passes (off unless another
    // user has it). The position stays; togglePause() restarts from it.
    saveProgress();
    stopStream();
    wifiParked = true;
    message = WIFI_PARKED_MESSAGE;
    AudioModule::logf("Player: paused %lu min, Wi-Fi released", static_cast<unsigned long>(PAUSED_WIFI_OFF_MS / 60000));
  }
  if (retryStreamAtMs != 0 && static_cast<int32_t>(millis() - retryStreamAtMs) >= 0) startStream(positionMs);
  // The Wi-Fi hold resumeAfterScreen() took over: the resumed stream holds its own by now.
  logStreamHealth();
  if (handedWifiAtMs != 0 && millis() - handedWifiAtMs >= HANDED_WIFI_HOLD_MS) {
    handedWifiAtMs = 0;
    AbsWifi::release();
  }

  updateFromFeeder();
  updateFromModule();
  refill();
  applySyncResult();
  if (!paused && streamActive && millis() - lastSaveMs >= SAVE_INTERVAL_MS) saveProgress();
  if (isAbsBook() && !paused && streamActive &&
      millis() - lastPushMs >= (remote ? REMOTE_PUSH_INTERVAL_MS : LOCAL_PUSH_INTERVAL_MS)) {
    if (remote) {
      requestPush();
    } else if (AbsWifi::connected()) {
      requestPush(true);
    }
  }
}

void shutdown() {
  if (!isOpen) return;
  saveProgress();
  stopStream();
}

}  // namespace AudioPlayer

#endif  // INX_HAS_AUDIO_MODULE
