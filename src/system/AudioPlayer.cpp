/**
 * @file AudioPlayer.cpp
 * @brief Definitions for the audiobook player.
 */

#include "AudioPlayer.h"

#if INX_HAS_AUDIO_MODULE

#include <Arduino.h>
#include <SDCardManager.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstring>
#include <memory>

#include "AudioLink.h"
#include "AudioModule.h"
#include "audio/AudioSource.h"
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

  bool push(const RecordKind kind, const uint8_t streamId, const uint8_t* data, const uint16_t length) {
    const RecordHeader header{kind, streamId, length};
    xSemaphoreTake(lock_, portMAX_DELAY);
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
// Player state (main loop only).

struct Progress {
  int track = 0;
  uint32_t ms = 0;
};

bool isOpen = false;
bool paused = false;
bool finished = false;
Book book;
std::vector<std::string> tracks;
int track = 0;
std::unique_ptr<AudioSource> source;
uint32_t trackDurationMs = 0;

uint8_t streamId = 0;
bool streamActive = false;
bool endQueued = false;
uint32_t streamStartMs = 0;
uint32_t positionMs = 0;
uint32_t session = 0;
uint32_t lastSaveMs = 0;
uint32_t lastModuleTryMs = 0;
std::string message;

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

// Progress file: one "<track>\t<ms>\t<book path>" line per book.
std::vector<std::pair<std::string, Progress>> loadAllProgress() {
  std::vector<std::pair<std::string, Progress>> entries;
  FsFile file = SdMan.open(PROGRESS_FILE, O_RDONLY);
  if (!file) return entries;
  std::string line;
  auto flush = [&]() {
    const size_t a = line.find('\t');
    const size_t b = a == std::string::npos ? std::string::npos : line.find('\t', a + 1);
    if (b != std::string::npos) {
      Progress p;
      p.track = std::atoi(line.substr(0, a).c_str());
      p.ms = static_cast<uint32_t>(std::strtoul(line.substr(a + 1, b - a - 1).c_str(), nullptr, 10));
      entries.emplace_back(line.substr(b + 1), p);
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
  SdIoMutex::Lock lock;
  auto entries = loadAllProgress();
  const Progress now{track, positionMs};
  bool found = false;
  for (auto& entry : entries) {
    if (entry.first == book.path) {
      entry.second = now;
      found = true;
    }
  }
  if (!found) entries.emplace_back(book.path, now);
  SdMan.mkdir("/.system");
  FsFile file = SdMan.open(PROGRESS_FILE, O_WRONLY | O_CREAT | O_TRUNC);
  if (!file) return;
  for (const auto& entry : entries) {
    char prefix[32];
    const int n = snprintf(prefix, sizeof(prefix), "%d\t%lu\t", entry.second.track,
                           static_cast<unsigned long>(entry.second.ms));
    file.write(prefix, n);
    file.write(entry.first.data(), entry.first.size());
    file.write("\n", 1);
  }
  file.close();
  lastSaveMs = millis();
}

void sendControl(const Type type, const void* payload = nullptr, const size_t length = 0) {
  if (AudioLink::ready()) AudioLink::send(type, payload, length, CONTROL_TIMEOUT_MS);
}

void stopStream() {
  queue.clear();
  if (streamActive) sendControl(Type::StreamStop, &streamId, 1);
  streamActive = false;
  endQueued = false;
}

bool openTrack(const int index) {
  source.reset();
  if (index < 0 || index >= static_cast<int>(tracks.size())) return false;
  track = index;
  auto next = AudioSource::forPath(tracks[index]);
  if (!next) {
    message = "Unsupported file: " + baseName(tracks[index]);
    return false;
  }
  SdIoMutex::Lock lock;
  if (!next->open(tracks[index])) {
    message = baseName(tracks[index]) + ": " + next->error();
    AudioModule::logf("Player: %s", message.c_str());
    return false;
  }
  trackDurationMs = next->durationMs();
  source = std::move(next);
  return true;
}

/** Starts a new stream at ms in the current track. The module drops anything of the old one. */
void startStream(uint32_t ms) {
  stopStream();
  if (!source) return;
  if (trackDurationMs && ms >= trackDurationMs) ms = trackDurationMs > 1000 ? trackDurationMs - 1000 : 0;
  {
    SdIoMutex::Lock lock;
    ms = source->seek(ms);
  }
  streamId = static_cast<uint8_t>(streamId + 1);
  streamStartMs = ms;
  positionMs = ms;
  finished = false;
  uint8_t start[5];
  start[0] = static_cast<uint8_t>(source->codec());
  std::memcpy(start + 1, &ms, sizeof(ms));
  queue.push(RecordKind::Start, streamId, start, sizeof(start));
  streamActive = true;
  sendControl(paused ? Type::Pause : Type::Resume);
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
  if (!streamActive || endQueued || !source) return;
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

void updateFromModule() {
  const AudioLink::ModuleStatus module = AudioLink::status();
  if (!module.valid || !streamActive || module.status.streamId != streamId) return;
  const auto state = static_cast<PlayState>(module.status.playState);
  if (state == PlayState::Idle) return;
  positionMs = streamStartMs + module.status.playedMs;
  if (trackDurationMs && positionMs > trackDurationMs) positionMs = trackDurationMs;

  if (state == PlayState::Finished && endQueued) {
    if (track + 1 < static_cast<int>(tracks.size()) && openTrack(track + 1)) {
      startStream(0);
      saveProgress();
    } else {
      streamActive = false;
      finished = true;
      positionMs = trackDurationMs;
      saveProgress();
    }
  }
}

}  // namespace

void begin() {
  if (!queue.begin()) {
    AudioModule::logf("Player: out of memory");
    return;
  }
  xTaskCreate(senderTask, "audio_send", 6144, nullptr, 3, nullptr);
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
      if (!listAudioFiles(path).empty()) books.push_back({path, name, true});
    } else if (AudioSource::isAudioFile(name)) {
      books.push_back({path, baseName(name), false});
    }
  }
  dir.close();
  std::sort(books.begin(), books.end(), [](const Book& a, const Book& b) { return naturalLess(a.title, b.title); });
  return books;
}

bool open(const Book& next) {
  stop();
  message.clear();
  book = next;
  {
    SdIoMutex::Lock lock;
    tracks = next.folder ? listAudioFiles(next.path) : std::vector<std::string>{next.path};
  }
  if (tracks.empty()) {
    message = "No audio files";
    return false;
  }
  Progress saved;
  {
    SdIoMutex::Lock lock;
    const auto entries = loadAllProgress();
    const auto match = std::find_if(entries.begin(), entries.end(),
                                    [&next](const auto& entry) { return entry.first == next.path; });
    if (match != entries.end()) saved = match->second;
  }
  if (saved.track < 0 || saved.track >= static_cast<int>(tracks.size())) saved = {};
  if (!openTrack(saved.track)) return false;
  isOpen = true;
  paused = false;
  finished = false;
  positionMs = saved.ms;
  lastModuleTryMs = 0;
  wakeModule();
  session = AudioLink::sessionCounter();
  if (AudioLink::ready()) startStream(saved.ms);
  AudioModule::logf("Player: %s, track %d/%d at %lu s", book.title.c_str(), track + 1,
                    static_cast<int>(tracks.size()), static_cast<unsigned long>(saved.ms / 1000));
  return true;
}

void stop() {
  if (!isOpen) return;
  saveProgress();
  stopStream();
  source.reset();
  isOpen = false;
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
  sendControl(paused ? Type::Pause : Type::Resume);
  if (paused) saveProgress();
}

void skip(const int32_t deltaMs) {
  if (!isOpen || !source) return;
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
}

void previousTrack() {
  if (!isOpen) return;
  if (positionMs > PREVIOUS_RESTARTS_TRACK_MS || track == 0) {
    startStream(0);
    return;
  }
  if (openTrack(track - 1)) startStream(0);
  saveProgress();
}

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
  v.waitingForModule = isOpen && !AudioLink::ready();
  v.title = book.title;
  v.trackName = isOpen && track < static_cast<int>(tracks.size()) ? baseName(tracks[track]) : "";
  v.track = track;
  v.trackCount = static_cast<int>(tracks.size());
  v.positionMs = positionMs;
  v.durationMs = trackDurationMs;
  v.queuedBytes = static_cast<uint32_t>(queue.usedBytes());
  v.message = message;
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

void service() {
  if (!isOpen) return;
  if (!paused && !finished) wakeModule();

  const uint32_t now = AudioLink::sessionCounter();
  if (now != session && AudioLink::ready()) {
    // New session: the module (re)connected or rebooted and knows nothing of our stream.
    session = now;
    if (!finished) startStream(positionMs);
  }

  updateFromModule();
  refill();
  if (!paused && streamActive && millis() - lastSaveMs >= SAVE_INTERVAL_MS) saveProgress();
}

void shutdown() {
  if (!isOpen) return;
  saveProgress();
  stopStream();
}

}  // namespace AudioPlayer

#endif  // INX_HAS_AUDIO_MODULE
