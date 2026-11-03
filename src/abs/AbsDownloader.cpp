/**
 * @file AbsDownloader.cpp
 * @brief Definitions for the Audiobookshelf book downloader.
 */

#include "AbsDownloader.h"

#if INX_HAS_AUDIO_MODULE

#include <Arduino.h>
#include <SDCardManager.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <atomic>
#include <memory>

#include "AbsLocal.h"
#include "AbsWifi.h"
#include "audio/HttpAudioFile.h"
#include "util/SdIoMutex.h"

namespace AbsDownloader {

namespace {

constexpr size_t CHUNK_BYTES = 64 * 1024;
constexpr int SLOTS = 3;
constexpr int WRITES_PER_SERVICE = 2;
constexpr uint32_t SPEED_WINDOW_MS = 3000;

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
TaskHandle_t task = nullptr;
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

void setError(const std::string& error) {
  xSemaphoreTake(lock, portMAX_DELAY);
  current.failed = true;
  current.error = error;
  xSemaphoreGive(lock);
}

void downloadTask(void*) {
  std::string error;
  {
    AbsWifi::Use wifi(error);
    for (size_t i = 0; wifi.ok() && i < meta.tracks.size() && !stopRequested && error.empty(); ++i) {
      const Abs::Track& t = meta.tracks[i];
      uint64_t offset = startAt[i];
      if (offset >= t.sizeBytes) continue;
      HttpAudioFile http(Abs::url(t.contentUrl));
      if (!http.open(error)) break;
      http.seekSet(offset);
      while (offset < t.sizeBytes && !stopRequested) {
        int slotIndex;
        if (xQueueReceive(freeSlots, &slotIndex, pdMS_TO_TICKS(200)) != pdTRUE) continue;
        Slot& slot = slots[slotIndex];
        const int n = http.read(slot.data, CHUNK_BYTES);
        if (n <= 0) {
          xQueueSend(freeSlots, &slotIndex, 0);
          if (!stopRequested) error = http.failed() ? http.lastError() : "file ended early";
          break;
        }
        slot.track = static_cast<int>(i);
        slot.offset = offset;
        slot.length = static_cast<size_t>(n);
        offset += static_cast<uint64_t>(n);
        xQueueSend(filledSlots, &slotIndex, portMAX_DELAY);
      }
    }
    if (!wifi.ok() && error.empty()) error = "Wi-Fi unavailable";
  }
  if (!error.empty() && !stopRequested) setError(error);
  taskFinished = error.empty() && !stopRequested;
  taskRunning = false;
  task = nullptr;
  vTaskDelete(nullptr);
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

}  // namespace

bool start(const Abs::ItemDetail& item, std::string& error) {
  // A task told to stop finishes its current chunk first; wait for it rather than run two.
  for (int i = 0; i < 100 && taskRunning && stopRequested; ++i) vTaskDelay(pdMS_TO_TICKS(50));
  if (taskRunning) return true;
  if (!lock) {
    lock = xSemaphoreCreateMutex();
    freeSlots = xQueueCreate(SLOTS, sizeof(int));
    filledSlots = xQueueCreate(SLOTS, sizeof(int));
    for (Slot& slot : slots) {
      slot.data = static_cast<uint8_t*>(heap_caps_malloc(CHUNK_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
      if (!slot.data) {
        error = "Out of memory";
        return false;
      }
    }
  }
  xQueueReset(freeSlots);
  xQueueReset(filledSlots);
  for (int i = 0; i < SLOTS; ++i) xQueueSend(freeSlots, &i, 0);

  // Same folder as an earlier, interrupted download of this item; otherwise a new one.
  AbsLocal::BookMeta existing;
  const bool resume = AbsLocal::find(item.id, existing);
  meta = AbsLocal::BookMeta{};
  meta.folder = resume ? existing.folder : AbsLocal::newFolder(item.title);
  meta.itemId = item.id;
  meta.title = item.title;
  meta.author = item.author;
  meta.durationMs = item.durationMs;
  meta.complete = false;
  for (Abs::Track track : item.tracks) {
    track.filename = AbsLocal::localName(track);
    meta.tracks.push_back(track);
  }
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

  stopRequested = false;
  taskFinished = false;
  taskRunning = true;
  if (xTaskCreate(downloadTask, "abs_download", 12288, nullptr, 2, &task) != pdPASS) {
    taskRunning = false;
    error = "Cannot start the download";
    return false;
  }
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
  for (int i = 0; i < WRITES_PER_SERVICE; ++i) {
    int slotIndex;
    if (xQueueReceive(filledSlots, &slotIndex, 0) != pdTRUE) break;
    const Slot& slot = slots[slotIndex];
    const bool ok = writeSlot(slot);
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

  // Wrap up once the task is gone and every buffer is on the card.
  xSemaphoreTake(lock, portMAX_DELAY);
  const bool wasActive = current.active;
  xSemaphoreGive(lock);
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

}  // namespace AbsDownloader

#endif  // INX_HAS_AUDIO_MODULE
