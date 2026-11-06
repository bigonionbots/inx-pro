/**
 * @file AbsQueue.cpp
 * @brief Definitions for the Audiobookshelf download queue.
 */

#include "AbsQueue.h"

#if INX_HAS_AUDIO_MODULE

#include <Arduino.h>
#include <ArduinoJson.h>
#include <SDCardManager.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>
#include <freertos/task.h>

#include <algorithm>
#include <atomic>

#include "AbsClient.h"
#include "AbsDownloader.h"
#include "AbsLocal.h"
#include "AbsWifi.h"
#include "system/Bq25896.h"
#include "util/SdIoMutex.h"

namespace AbsQueue {

namespace {

constexpr const char* QUEUE_FILE = "/.system/abs_queue.json";
constexpr uint32_t CHARGER_POLL_MS = 5000;
/** A failed book (no Wi-Fi, server down) is tried again after this. */
constexpr uint32_t RETRY_AFTER_MS = 10 * 60 * 1000;
constexpr uint32_t FETCH_STACK = 12288;  // TLS

std::vector<Entry> queue;
std::vector<uint32_t> failedAt;  // parallel to queue: when it failed (0 = not failed)
uint32_t changes = 0;
bool onCharger = false;
uint32_t lastChargerPoll = 0;
std::string activeId;  // the book the downloader is on, started by the queue

// Details fetch: the main loop asks (fetchId, then notifies the task), the task answers.
TaskHandle_t fetchTask = nullptr;
std::string fetchId;                   // main loop writes before notifying; the task reads
std::atomic<bool> fetching{false};     // a fetch is in flight
std::atomic<bool> fetchDone{false};    // its result is ready
Abs::ItemDetail fetchedDetail;         // written by the task before fetchDone
std::string fetchError;                // ditto
std::atomic<bool> wifiHeld{false};     // the queue's share of Wi-Fi, from the fetch to the download's end

void changed() { ++changes; }

void save() {
  JsonDocument doc;
  for (const Entry& e : queue) {
    JsonObject o = doc.add<JsonObject>();
    o["id"] = e.itemId;
    o["title"] = e.title;
    o["author"] = e.author;
    o["now"] = e.now;
  }
  std::string text;
  serializeJson(doc, text);
  SdIoMutex::Lock lock;
  SdMan.mkdir("/.system");
  FsFile file = SdMan.open(QUEUE_FILE, O_WRONLY | O_CREAT | O_TRUNC);
  if (!file) return;
  file.write(text.data(), text.size());
  file.close();
}

int find(const std::string& id) {
  for (size_t i = 0; i < queue.size(); ++i) {
    if (queue[i].itemId == id) return static_cast<int>(i);
  }
  return -1;
}

void releaseWifi() {
  if (wifiHeld.exchange(false)) AbsWifi::release();
}

void fetchMain(void*) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    std::string error;
    Abs::ItemDetail detail;
    // Hold Wi-Fi from here until the download ends (releaseWifi(), main loop), so it is not
    // dropped between fetching the details and the downloader connecting.
    if (!wifiHeld && AbsWifi::acquire(error)) wifiHeld = true;
    if (wifiHeld) Abs::item(fetchId, detail, error);
    fetchedDetail = std::move(detail);
    fetchError = error;
    fetchDone = true;
    fetching = false;
  }
}

bool readCharger() {
  if (!Bq25896::present()) return false;
  Bq25896::Status status;
  return Bq25896::readStatus(status) && Bq25896::externalVbusPresent(status);
}

void markFailed(const int i, const std::string& error) {
  queue[i].state = State::Failed;
  queue[i].error = error;
  failedAt[i] = millis();
  changed();
}

/** Starts fetching the details of the first book allowed to run now. */
void startNext() {
  if (!activeId.empty() || fetching || fetchDone || AbsDownloader::status().active) return;
  const uint32_t now = millis();
  for (size_t i = 0; i < queue.size(); ++i) {
    Entry& e = queue[i];
    if (!(e.now || onCharger)) continue;
    if (e.state == State::Failed) {
      if (now - failedAt[i] < RETRY_AFTER_MS) continue;
      e.state = State::Waiting;
    }
    if (e.state != State::Waiting) continue;
    if (!fetchTask &&
        xTaskCreatePinnedToCoreWithCaps(fetchMain, "abs_queue", FETCH_STACK, nullptr, 1, &fetchTask, tskNO_AFFINITY,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
      fetchTask = nullptr;
      return;
    }
    e.state = State::Starting;
    e.error.clear();
    fetchId = e.itemId;
    fetching = true;
    changed();
    xTaskNotifyGive(fetchTask);
    return;
  }
}

}  // namespace

void begin() {
  queue.clear();
  SdIoMutex::Lock lock;
  FsFile file = SdMan.open(QUEUE_FILE, O_RDONLY);
  if (file) {
    std::string text;
    char chunk[256];
    for (int n; (n = file.read(chunk, sizeof(chunk))) > 0;) text.append(chunk, static_cast<size_t>(n));
    file.close();
    JsonDocument doc;
    if (deserializeJson(doc, text) == DeserializationError::Ok) {
      for (JsonObject o : doc.as<JsonArray>()) {
        Entry e;
        e.itemId = o["id"] | "";
        e.title = o["title"] | "";
        e.author = o["author"] | "";
        e.now = o["now"] | false;
        if (!e.itemId.empty()) queue.push_back(e);
      }
    }
  }
  failedAt.assign(queue.size(), 0);
}

void service() {
  AbsDownloader::service();  // writes downloaded buffers to the card, whatever screen is open

  const uint32_t now = millis();
  if (now - lastChargerPoll >= CHARGER_POLL_MS || lastChargerPoll == 0) {
    lastChargerPoll = now;
    const bool was = onCharger;
    onCharger = readCharger();
    if (was != onCharger) changed();
  }

  // The details came back: start the download (or note why not).
  if (fetchDone.exchange(false)) {
    const int i = find(fetchId);
    if (i < 0) {
      releaseWifi();  // removed meanwhile
    } else if (!fetchError.empty()) {
      releaseWifi();
      markFailed(i, fetchError);
    } else {
      std::string error;
      if (AbsDownloader::start(fetchedDetail, error)) {
        queue[i].state = State::Downloading;
        activeId = queue[i].itemId;
        changed();
      } else {
        releaseWifi();
        markFailed(i, error);
      }
    }
    fetchedDetail = Abs::ItemDetail{};
  }

  // The running download: finished, failed, or no longer allowed (charger gone).
  if (!activeId.empty()) {
    const AbsDownloader::Status s = AbsDownloader::status();
    const int i = find(activeId);
    if (i < 0) {
      activeId.clear();
      releaseWifi();
    } else if (s.itemId == activeId && s.done) {
      queue.erase(queue.begin() + i);  // complete: on the device now
      failedAt.erase(failedAt.begin() + i);
      activeId.clear();
      releaseWifi();
      save();
      changed();
    } else if (s.itemId == activeId && s.failed) {
      activeId.clear();
      releaseWifi();
      markFailed(i, s.error);
    } else if (!s.active) {
      queue[i].state = State::Waiting;  // stopped from elsewhere
      activeId.clear();
      releaseWifi();
      changed();
    } else if (!(queue[i].now || onCharger)) {
      AbsDownloader::pause();  // off the charger: wait for it again
      queue[i].state = State::Waiting;
      activeId.clear();
      releaseWifi();
      changed();
    }
  }

  startNext();
}

bool keepAwake() {
  if (!onCharger) return false;
  if (!activeId.empty() || fetching) return true;
  return std::any_of(queue.begin(), queue.end(), [](const Entry& e) { return e.state == State::Waiting; });
}

void shutdown() {
  if (!activeId.empty()) AbsDownloader::pause();
}

std::vector<Entry> entries() { return queue; }

bool contains(const std::string& itemId) { return find(itemId) >= 0; }

void add(const std::string& itemId, const std::string& title, const std::string& author, const bool now) {
  const int i = find(itemId);
  if (i >= 0) {
    if (now && !queue[i].now) {
      queue[i].now = true;
      save();
      changed();
    }
    return;
  }
  Entry e;
  e.itemId = itemId;
  e.title = title;
  e.author = author;
  e.now = now;
  queue.push_back(e);
  failedAt.push_back(0);
  save();
  changed();
}

void remove(const std::string& itemId) {
  const int i = find(itemId);
  if (i < 0) return;
  if (activeId == itemId) {
    AbsDownloader::pause();
    activeId.clear();
    releaseWifi();
  }
  queue.erase(queue.begin() + i);
  failedAt.erase(failedAt.begin() + i);
  // What it had downloaded so far goes too (a complete copy is not the queue's to delete).
  AbsLocal::BookMeta meta;
  if (AbsLocal::find(itemId, meta) && !meta.complete) AbsLocal::remove(meta);
  save();
  changed();
}

void move(const std::string& itemId, const int delta) {
  const int i = find(itemId);
  const int j = i + delta;
  if (i < 0 || j < 0 || j >= static_cast<int>(queue.size())) return;
  std::swap(queue[i], queue[j]);
  std::swap(failedAt[i], failedAt[j]);
  save();
  changed();
}

void startNow(const std::string& itemId) {
  const int i = find(itemId);
  if (i < 0) return;
  Entry e = queue[i];
  const uint32_t failed = failedAt[i];
  queue.erase(queue.begin() + i);
  failedAt.erase(failedAt.begin() + i);
  e.now = true;
  if (e.state == State::Failed) e.state = State::Waiting;  // try again at once
  queue.insert(queue.begin(), e);
  failedAt.insert(failedAt.begin(), e.state == State::Failed ? failed : 0);
  save();
  changed();
}

bool charging() { return onCharger; }

uint32_t changeCounter() { return changes; }

}  // namespace AbsQueue

#endif  // INX_HAS_AUDIO_MODULE
