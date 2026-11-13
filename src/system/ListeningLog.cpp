/**
 * @file ListeningLog.cpp
 * @brief Audiobook listening time: local stats for every book, and Audiobookshelf sessions to upload.
 */

#include "ListeningLog.h"

#if INX_HAS_AUDIO_MODULE

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HalGPIO.h>
#include <SDCardManager.h>
#include <esp_random.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "state/SystemSetting.h"
#include "util/SdIoMutex.h"

extern HalGPIO gpio;

namespace ListeningLog {

namespace {

constexpr const char* STATS_FILE = "/.system/audio_stats.json";
constexpr const char* SESSIONS_FILE = "/.system/abs_sessions.json";
constexpr size_t DAYS_KEPT = 400;
constexpr size_t BOOKS_KEPT = 50;
constexpr size_t SESSIONS_KEPT = 100;  // uploaded ones go first when there are more
constexpr size_t UPLOADS_PER_SYNC = 4;
/** Listening after a pause this long is a new session (the apps start one per play). */
constexpr uint64_t SESSION_GAP_MS = 30ull * 60 * 1000;

struct Session {
  std::string id;
  std::string itemId;
  std::string title;
  std::string author;
  uint32_t dateKey = 0;
  uint64_t startedAtMs = 0;  // UTC epoch
  uint64_t updatedAtMs = 0;
  uint32_t startMs = 0;  // position in the book
  uint32_t currentMs = 0;
  uint32_t durationMs = 0;
  uint32_t listenedMs = 0;
  uint32_t uploadedMs = 0;  // listening time the server has
  bool open = false;        // the current one: more listening goes into it
};

bool loaded = false;
Stats local;
std::vector<Session> sessions;

struct Clock {
  uint32_t dateKey = 0;   // YYYYMMDD, local
  uint64_t utcMs = 0;     // epoch
  uint8_t dayOfWeek = 0;  // 0 = Sunday
};

int64_t daysFromCivil(int year, const unsigned month, const unsigned day) {
  year -= month <= 2;
  const int era = (year >= 0 ? year : year - 399) / 400;
  const unsigned yearOfEra = static_cast<unsigned>(year - era * 400);
  const unsigned dayOfYear = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
  const unsigned dayOfEra = yearOfEra * 365 + yearOfEra / 4 - yearOfEra / 100 + dayOfYear;
  return static_cast<int64_t>(era) * 146097 + static_cast<int64_t>(dayOfEra) - 719468;
}

bool now(Clock& out) {
  HalGPIO::DateTime t;
  if (!gpio.readDateTime(t) || t.year < 2020 || t.month < 1 || t.month > 12 || t.day < 1 || t.day > 31) {
    return false;
  }
  const int64_t days = daysFromCivil(t.year, t.month, t.day);
  const int64_t localSeconds = days * 86400 + t.hour * 3600 + t.minute * 60 + t.second;
  out.dateKey = static_cast<uint32_t>(t.year) * 10000 + t.month * 100 + t.day;
  out.utcMs = static_cast<uint64_t>(localSeconds - SETTINGS.getTimeZoneOffsetMinutes() * 60LL) * 1000;
  out.dayOfWeek = static_cast<uint8_t>(((days % 7) + 7 + 4) % 7);  // 1970-01-01 was a Thursday
  return true;
}

std::string newId() {
  // A random UUID (v4): the server keys sessions by it.
  uint8_t b[16];
  for (int i = 0; i < 16; i += 4) {
    const uint32_t r = esp_random();
    std::memcpy(b + i, &r, 4);
  }
  b[6] = static_cast<uint8_t>((b[6] & 0x0F) | 0x40);
  b[8] = static_cast<uint8_t>((b[8] & 0x3F) | 0x80);
  char text[37];
  snprintf(text, sizeof(text), "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", b[0], b[1], b[2],
           b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
  return text;
}

std::string deviceId() {
  char text[32];
  snprintf(text, sizeof(text), "inx-pro-%012llx", static_cast<unsigned long long>(ESP.getEfuseMac()));
  return text;
}

std::string readFile(const char* path) {
  std::string text;
  SdIoMutex::Lock lock;
  FsFile file = SdMan.open(path, O_RDONLY);
  if (!file) return text;
  char chunk[256];
  for (int n; (n = file.read(chunk, sizeof(chunk))) > 0;) text.append(chunk, static_cast<size_t>(n));
  file.close();
  return text;
}

void writeFile(const char* path, const std::string& text) {
  SdIoMutex::Lock lock;
  SdMan.mkdir("/.system");
  FsFile file = SdMan.open(path, O_WRONLY | O_CREAT | O_TRUNC);
  if (!file) return;
  file.write(text.data(), text.size());
  file.close();
}

void load() {
  if (loaded) return;
  loaded = true;
  JsonDocument doc;
  if (deserializeJson(doc, readFile(STATS_FILE)) == DeserializationError::Ok) {
    local.totalSeconds = doc["total"] | 0ull;
    for (JsonArray d : doc["days"].as<JsonArray>()) local.days.push_back({d[0] | 0u, d[1] | 0u});
    for (JsonObject b : doc["books"].as<JsonArray>()) {
      BookTotal book;
      book.key = b["key"] | "";
      book.title = b["title"] | "";
      book.author = b["author"] | "";
      book.seconds = b["s"] | 0u;
      book.lastDateKey = b["last"] | 0u;
      if (!book.key.empty()) local.books.push_back(book);
    }
  }
  doc.clear();
  if (deserializeJson(doc, readFile(SESSIONS_FILE)) == DeserializationError::Ok) {
    for (JsonObject o : doc.as<JsonArray>()) {
      Session s;
      s.id = o["id"] | "";
      s.itemId = o["item"] | "";
      s.title = o["title"] | "";
      s.author = o["author"] | "";
      s.dateKey = o["date"] | 0u;
      s.startedAtMs = o["started"] | 0ull;
      s.updatedAtMs = o["updated"] | 0ull;
      s.startMs = o["start"] | 0u;
      s.currentMs = o["current"] | 0u;
      s.durationMs = o["duration"] | 0u;
      s.listenedMs = o["listened"] | 0u;
      s.uploadedMs = o["uploaded"] | 0u;
      s.open = false;  // a reboot ends a session
      if (!s.id.empty() && !s.itemId.empty()) sessions.push_back(s);
    }
  }
}

void saveStats() {
  JsonDocument doc;
  doc["total"] = local.totalSeconds;
  JsonArray days = doc["days"].to<JsonArray>();
  for (const DayTotal& d : local.days) {
    JsonArray entry = days.add<JsonArray>();
    entry.add(d.dateKey);
    entry.add(d.seconds);
  }
  JsonArray books = doc["books"].to<JsonArray>();
  for (const BookTotal& b : local.books) {
    JsonObject o = books.add<JsonObject>();
    o["key"] = b.key;
    o["title"] = b.title;
    o["author"] = b.author;
    o["s"] = b.seconds;
    o["last"] = b.lastDateKey;
  }
  std::string text;
  serializeJson(doc, text);
  writeFile(STATS_FILE, text);
}

void saveSessions() {
  JsonDocument doc;
  for (const Session& s : sessions) {
    JsonObject o = doc.add<JsonObject>();
    o["id"] = s.id;
    o["item"] = s.itemId;
    o["title"] = s.title;
    o["author"] = s.author;
    o["date"] = s.dateKey;
    o["started"] = s.startedAtMs;
    o["updated"] = s.updatedAtMs;
    o["start"] = s.startMs;
    o["current"] = s.currentMs;
    o["duration"] = s.durationMs;
    o["listened"] = s.listenedMs;
    o["uploaded"] = s.uploadedMs;
  }
  std::string text;
  serializeJson(doc, text);
  writeFile(SESSIONS_FILE, text);
}

/** Leftover milliseconds per book, so seconds are counted without losing the remainders. */
uint32_t carryMs = 0;

void addLocal(const BookInfo& book, const uint32_t listenedMs, const uint32_t dateKey) {
  carryMs += listenedMs;
  const uint32_t seconds = carryMs / 1000;
  carryMs %= 1000;
  if (seconds == 0) return;
  local.totalSeconds += seconds;
  if (dateKey != 0) {
    if (!local.days.empty() && local.days.back().dateKey == dateKey) {
      local.days.back().seconds += seconds;
    } else {
      local.days.push_back({dateKey, seconds});
      if (local.days.size() > DAYS_KEPT) local.days.erase(local.days.begin());
    }
  }
  BookTotal entry;
  for (auto it = local.books.begin(); it != local.books.end(); ++it) {
    if (it->key == book.key) {
      entry = *it;
      local.books.erase(it);
      break;
    }
  }
  entry.key = book.key;
  entry.title = book.title;
  if (!book.author.empty()) entry.author = book.author;
  entry.seconds += seconds;
  if (dateKey != 0) entry.lastDateKey = dateKey;
  local.books.insert(local.books.begin(), entry);
  if (local.books.size() > BOOKS_KEPT) local.books.pop_back();
}

void addSession(const BookInfo& book, const uint32_t listenedMs, const Clock& clock) {
  Session* current = nullptr;
  for (Session& s : sessions) {
    if (s.open) current = &s;
  }
  if (current && (current->itemId != book.itemId || current->dateKey != clock.dateKey ||
                  clock.utcMs > current->updatedAtMs + SESSION_GAP_MS)) {
    current->open = false;
    current = nullptr;
  }
  if (!current) {
    Session s;
    s.id = newId();
    s.itemId = book.itemId;
    s.dateKey = clock.dateKey;
    s.startedAtMs = clock.utcMs > listenedMs ? clock.utcMs - listenedMs : clock.utcMs;
    s.startMs = book.positionMs > listenedMs ? book.positionMs - listenedMs : 0;
    s.open = true;
    sessions.push_back(s);
    current = &sessions.back();
  }
  current->title = book.title;
  current->author = book.author;
  current->updatedAtMs = clock.utcMs;
  current->currentMs = book.positionMs;
  if (book.durationMs) current->durationMs = book.durationMs;
  current->listenedMs += listenedMs;

  // Keep the file small: drop uploaded, finished sessions first, then the oldest.
  while (sessions.size() > SESSIONS_KEPT) {
    auto done = std::find_if(sessions.begin(), sessions.end(),
                             [](const Session& s) { return !s.open && s.uploadedMs >= s.listenedMs; });
    sessions.erase(done != sessions.end() ? done : sessions.begin());
  }
}

const char* dayName(const uint8_t day) {
  static const char* names[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
  return names[day % 7];
}

std::string sessionJson(const Session& s) {
  JsonDocument doc;
  doc["id"] = s.id;
  const size_t slash = s.itemId.find('/');
  doc["libraryItemId"] = s.itemId.substr(0, slash);
  if (slash != std::string::npos) {
    doc["episodeId"] = s.itemId.substr(slash + 1);
    doc["mediaType"] = "podcast";
  } else {
    doc["mediaType"] = "book";
  }
  doc["displayTitle"] = s.title;
  doc["displayAuthor"] = s.author;
  doc["duration"] = s.durationMs / 1000.0;
  doc["playMethod"] = 3;  // local
  doc["mediaPlayer"] = "inx-pro";
  JsonObject device = doc["deviceInfo"].to<JsonObject>();
  device["deviceId"] = deviceId();
  device["clientName"] = "Inx Pro";
  device["clientVersion"] = INX_VERSION;
  device["manufacturer"] = "LilyGo";
  device["model"] = "T5 S3";
  char date[11];
  snprintf(date, sizeof(date), "%04lu-%02lu-%02lu", static_cast<unsigned long>(s.dateKey / 10000),
           static_cast<unsigned long>(s.dateKey / 100 % 100), static_cast<unsigned long>(s.dateKey % 100));
  doc["date"] = date;
  const int64_t days = daysFromCivil(static_cast<int>(s.dateKey / 10000), s.dateKey / 100 % 100, s.dateKey % 100);
  doc["dayOfWeek"] = dayName(static_cast<uint8_t>(((days % 7) + 7 + 4) % 7));
  doc["timeListening"] = s.listenedMs / 1000.0;
  doc["startTime"] = s.startMs / 1000.0;
  doc["currentTime"] = s.currentMs / 1000.0;
  doc["startedAt"] = s.startedAtMs;
  doc["updatedAt"] = s.updatedAtMs;
  std::string body;
  serializeJson(doc, body);
  return body;
}

}  // namespace

uint32_t todayKey() {
  Clock clock;
  return now(clock) ? clock.dateKey : 0;
}

void record(const BookInfo& book, const uint32_t listenedMs) {
  if (listenedMs == 0 || book.key.empty()) return;
  load();
  Clock clock;
  const bool dated = now(clock);
  addLocal(book, listenedMs, dated ? clock.dateKey : 0);
  saveStats();
  // The server needs a date and a time for a session; without the clock only local stats count it.
  if (!book.itemId.empty() && dated) {
    addSession(book, listenedMs, clock);
    saveSessions();
  }
}

void endSession() {
  if (!loaded) return;
  for (Session& s : sessions) s.open = false;
}

std::vector<Upload> pendingUploads() {
  load();
  std::vector<Upload> out;
  for (const Session& s : sessions) {
    if (s.listenedMs <= s.uploadedMs || s.listenedMs < 1000) continue;
    out.push_back({s.id, s.itemId, sessionJson(s), s.listenedMs});
    if (out.size() >= UPLOADS_PER_SYNC) break;
  }
  return out;
}

void uploaded(const std::string& id, const uint32_t listenedMs) {
  load();
  for (Session& s : sessions) {
    if (s.id == id) s.uploadedMs = std::max(s.uploadedMs, listenedMs);
  }
  // Finished and fully uploaded: nothing more to send.
  sessions.erase(std::remove_if(sessions.begin(), sessions.end(),
                                [](const Session& s) { return !s.open && s.uploadedMs >= s.listenedMs; }),
                 sessions.end());
  saveSessions();
}

Stats stats() {
  load();
  return local;
}

}  // namespace ListeningLog

#endif  // INX_HAS_AUDIO_MODULE
