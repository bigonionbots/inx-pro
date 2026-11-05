/**
 * @file AbsLibrary.cpp
 * @brief Definitions for the Audiobookshelf screen's SD-card cache.
 */

#include "AbsLibrary.h"

#if INX_HAS_AUDIO_MODULE

#include <ArduinoJson.h>
#include <SDCardManager.h>
#include <esp_heap_caps.h>

#include "util/SdIoMutex.h"

namespace AbsLibrary {

namespace {

constexpr const char* DIRECTORY = "/.system/cache/abs";
constexpr size_t MAX_FILE_BYTES = 8 * 1024 * 1024;

std::string pathOf(const std::string& name) { return std::string(DIRECTORY) + "/" + name; }

/** The whole file, in PSRAM (empty when missing or too large). */
AbsIndex::PsramVector<uint8_t> readFile(const std::string& path) {
  AbsIndex::PsramVector<uint8_t> data;
  SdIoMutex::Lock lock;
  FsFile file = SdMan.open(path.c_str(), O_RDONLY);
  if (!file) return data;
  const size_t size = static_cast<size_t>(file.fileSize());
  if (size > 0 && size <= MAX_FILE_BYTES) {
    data.resize(size);
    if (file.read(data.data(), size) != static_cast<int>(size)) data.clear();
  }
  file.close();
  return data;
}

void writeFile(const std::string& path, const uint8_t* data, const size_t length) {
  SdIoMutex::Lock lock;
  SdMan.mkdir("/.system/cache");
  SdMan.mkdir(DIRECTORY);
  const std::string temporary = path + ".tmp";
  FsFile file = SdMan.open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC);
  bool ok = file && file.write(data, length) == length;
  if (file) file.close();
  if (ok) {
    SdMan.remove(path.c_str());
    ok = SdMan.rename(temporary.c_str(), path.c_str());
  }
  if (!ok) SdMan.remove(temporary.c_str());
}

void writeJson(const std::string& path, const JsonDocument& doc) {
  std::string text;
  serializeJson(doc, text);
  writeFile(path, reinterpret_cast<const uint8_t*>(text.data()), text.size());
}

}  // namespace

bool loadLibraries(std::vector<Abs::Library>& out) {
  const auto data = readFile(pathOf("libraries.json"));
  JsonDocument doc;
  if (data.empty() || deserializeJson(doc, data.data(), data.size()) != DeserializationError::Ok) return false;
  out.clear();
  for (JsonObject l : doc.as<JsonArray>()) out.push_back({l["id"] | "", l["name"] | ""});
  return !out.empty();
}

void saveLibraries(const std::vector<Abs::Library>& libraries) {
  JsonDocument doc;
  for (const Abs::Library& l : libraries) {
    JsonObject o = doc.add<JsonObject>();
    o["id"] = l.id;
    o["name"] = l.name;
  }
  writeJson(pathOf("libraries.json"), doc);
}

bool loadShelves(const std::string& libraryId, std::vector<Abs::Shelf>& out) {
  const auto data = readFile(pathOf(libraryId + "-home.json"));
  JsonDocument doc;
  if (data.empty() || deserializeJson(doc, data.data(), data.size()) != DeserializationError::Ok) return false;
  out.clear();
  for (JsonObject s : doc.as<JsonArray>()) {
    Abs::Shelf shelf;
    shelf.id = s["id"] | "";
    shelf.label = s["label"] | "";
    for (JsonObject b : s["items"].as<JsonArray>()) {
      Abs::ItemSummary item;
      item.id = b["id"] | "";
      item.title = b["title"] | "";
      item.author = b["author"] | "";
      item.durationSec = b["duration"] | 0u;
      item.sizeBytes = b["size"] | 0ull;
      item.progress = b["progress"] | 0.0f;
      item.finished = b["finished"] | false;
      shelf.items.push_back(item);
    }
    out.push_back(shelf);
  }
  return true;
}

void saveShelves(const std::string& libraryId, const std::vector<Abs::Shelf>& shelves) {
  JsonDocument doc;
  for (const Abs::Shelf& shelf : shelves) {
    JsonObject s = doc.add<JsonObject>();
    s["id"] = shelf.id;
    s["label"] = shelf.label;
    JsonArray items = s["items"].to<JsonArray>();
    for (const Abs::ItemSummary& item : shelf.items) {
      JsonObject b = items.add<JsonObject>();
      b["id"] = item.id;
      b["title"] = item.title;
      b["author"] = item.author;
      b["duration"] = item.durationSec;
      b["size"] = item.sizeBytes;
      b["progress"] = item.progress;
      b["finished"] = item.finished;
    }
  }
  writeJson(pathOf(libraryId + "-home.json"), doc);
}

bool loadIndex(const std::string& libraryId, AbsIndex& out) {
  const auto data = readFile(pathOf(libraryId + ".idx"));
  return !data.empty() && out.deserialize(data.data(), data.size());
}

void saveIndex(const std::string& libraryId, const AbsIndex& index) {
  const auto data = index.serialize();
  writeFile(pathOf(libraryId + ".idx"), data.data(), data.size());
}

void clear() {
  SdIoMutex::Lock lock;
  FsFile dir = SdMan.open(DIRECTORY, O_RDONLY);
  if (!dir) return;
  std::vector<std::string> names;
  char name[96];
  for (FsFile entry = dir.openNextFile(); entry; entry = dir.openNextFile()) {
    entry.getName(name, sizeof(name));
    entry.close();
    names.emplace_back(name);
  }
  dir.close();
  for (const std::string& n : names) SdMan.remove(pathOf(n).c_str());
}

}  // namespace AbsLibrary

#endif  // INX_HAS_AUDIO_MODULE
