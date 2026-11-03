/**
 * @file AbsLocal.cpp
 * @brief Definitions for the downloaded-book metadata.
 */

#include "AbsLocal.h"

#if INX_HAS_AUDIO_MODULE

#include <ArduinoJson.h>
#include <SDCardManager.h>

#include "system/AudioPlayer.h"
#include "util/SdIoMutex.h"

namespace AbsLocal {

namespace {

constexpr size_t MAX_NAME = 80;

std::string safeName(const std::string& name) {
  std::string out;
  for (const char c : name) {
    if (static_cast<unsigned char>(c) < 0x20 || std::string("\\/:*?\"<>|").find(c) != std::string::npos) {
      out += '_';
    } else {
      out += c;
    }
  }
  while (!out.empty() && (out.back() == ' ' || out.back() == '.')) out.pop_back();
  while (!out.empty() && out.front() == ' ') out.erase(out.begin());
  if (out.size() > MAX_NAME) out.resize(MAX_NAME);
  return out.empty() ? "Audiobook" : out;
}

std::string metaPath(const std::string& folder) { return folder + "/" + META_FILE; }

}  // namespace

bool read(const std::string& folder, BookMeta& out) {
  SdIoMutex::Lock lock;
  FsFile file = SdMan.open(metaPath(folder).c_str(), O_RDONLY);
  if (!file) return false;
  std::string text;
  char chunk[512];
  int got;
  while ((got = file.read(chunk, sizeof(chunk))) > 0) text.append(chunk, got);
  file.close();
  JsonDocument doc;
  if (deserializeJson(doc, text) != DeserializationError::Ok) return false;
  out = BookMeta{};
  out.folder = folder;
  out.itemId = doc["itemId"] | "";
  out.title = doc["title"] | "";
  out.author = doc["author"] | "";
  out.durationMs = doc["durationMs"] | 0u;
  out.complete = doc["complete"] | false;
  for (JsonObject t : doc["tracks"].as<JsonArray>()) {
    Abs::Track track;
    track.index = t["index"] | 0;
    track.filename = t["file"] | "";
    track.contentUrl = t["url"] | "";
    track.sizeBytes = t["size"] | 0ull;
    track.startOffsetMs = t["startMs"] | 0u;
    track.durationMs = t["durationMs"] | 0u;
    out.tracks.push_back(track);
  }
  return !out.itemId.empty() && !out.tracks.empty();
}

bool write(const BookMeta& meta) {
  JsonDocument doc;
  doc["itemId"] = meta.itemId;
  doc["title"] = meta.title;
  doc["author"] = meta.author;
  doc["durationMs"] = meta.durationMs;
  doc["complete"] = meta.complete;
  JsonArray tracks = doc["tracks"].to<JsonArray>();
  for (const Abs::Track& track : meta.tracks) {
    JsonObject t = tracks.add<JsonObject>();
    t["index"] = track.index;
    t["file"] = track.filename;
    t["url"] = track.contentUrl;
    t["size"] = track.sizeBytes;
    t["startMs"] = track.startOffsetMs;
    t["durationMs"] = track.durationMs;
  }
  std::string text;
  serializeJsonPretty(doc, text);
  SdIoMutex::Lock lock;
  SdMan.mkdir(meta.folder.c_str());
  FsFile file = SdMan.open(metaPath(meta.folder).c_str(), O_WRONLY | O_CREAT | O_TRUNC);
  if (!file) return false;
  const bool ok = file.write(text.data(), text.size()) == text.size();
  file.close();
  return ok;
}

namespace {

std::vector<std::string> bookFolders() {
  std::vector<std::string> folders;
  SdIoMutex::Lock lock;
  FsFile dir = SdMan.open(AudioPlayer::BOOKS_DIRECTORY, O_RDONLY);
  if (!dir || !dir.isDirectory()) return folders;
  char name[256];
  for (;;) {
    FsFile entry = dir.openNextFile();
    if (!entry) break;
    entry.getName(name, sizeof(name));
    const bool isDirectory = entry.isDirectory();
    entry.close();
    if (isDirectory && name[0] != '.') folders.push_back(std::string(AudioPlayer::BOOKS_DIRECTORY) + "/" + name);
  }
  dir.close();
  return folders;
}

}  // namespace

bool find(const std::string& itemId, BookMeta& out) {
  for (const std::string& folder : bookFolders()) {
    BookMeta meta;
    if (read(folder, meta) && meta.itemId == itemId) {
      out = meta;
      return true;
    }
  }
  return false;
}

std::vector<std::pair<std::string, bool>> listDownloaded() {
  std::vector<std::pair<std::string, bool>> items;
  for (const std::string& folder : bookFolders()) {
    BookMeta meta;
    if (read(folder, meta)) items.emplace_back(meta.itemId, meta.complete);
  }
  return items;
}

std::string newFolder(const std::string& title) {
  const std::string base = std::string(AudioPlayer::BOOKS_DIRECTORY) + "/" + safeName(title);
  SdIoMutex::Lock lock;
  if (!SdMan.exists(base.c_str())) return base;
  for (int n = 2; n < 100; ++n) {
    const std::string candidate = base + " (" + std::to_string(n) + ")";
    if (!SdMan.exists(candidate.c_str())) return candidate;
  }
  return base + " (abs)";
}

std::string localName(const Abs::Track& track) {
  std::string name = safeName(track.filename);
  if (name == "Audiobook") name = "Track " + std::to_string(track.index) + ".mp3";
  return name;
}

uint64_t localSize(const BookMeta& meta, const Abs::Track& track) {
  SdIoMutex::Lock lock;
  FsFile file = SdMan.open((meta.folder + "/" + track.filename).c_str(), O_RDONLY);
  if (!file) return 0;
  const uint64_t size = file.fileSize();
  file.close();
  return size;
}

bool remove(const BookMeta& meta) {
  SdIoMutex::Lock lock;
  bool ok = true;
  for (const Abs::Track& track : meta.tracks) {
    const std::string path = meta.folder + "/" + track.filename;
    if (SdMan.exists(path.c_str())) ok = SdMan.remove(path.c_str()) && ok;
  }
  SdMan.remove(metaPath(meta.folder).c_str());
  return SdMan.removeDir(meta.folder.c_str()) && ok;
}

}  // namespace AbsLocal

#endif  // INX_HAS_AUDIO_MODULE
