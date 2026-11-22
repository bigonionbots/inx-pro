/**
 * @file ReadAloudIndex.cpp
 * @brief Definitions for ReadAloudIndex.
 */

#include "ReadAloudIndex.h"

#if INX_HAS_AUDIO_MODULE

#include <Arduino.h>
#include <Epub.h>
#include <Epub/Section.h>
#include <FsHelpers.h>
#include <HardwareSerial.h>
#include <Print.h>
#include <SDCardManager.h>
#include <Serialization.h>
#include <expat.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

namespace {

constexpr uint32_t CACHE_MAGIC = 0x314c4152;  // "RAL1"
constexpr uint32_t CACHE_VERSION = 1;
constexpr const char* CACHE_NAME = "/readaloud.bin";
constexpr size_t SMIL_CHUNK = 4096;

/** The element name without a namespace prefix ("smil:par" -> "par"). */
const char* localName(const char* name) {
  const char* colon = strrchr(name, ':');
  return colon ? colon + 1 : name;
}

const char* attribute(const XML_Char** atts, const char* wanted) {
  for (int i = 0; atts[i]; i += 2) {
    if (strcmp(localName(atts[i]), wanted) == 0) return atts[i + 1];
  }
  return nullptr;
}

/**
 * A SMIL clock value in ms: "1.75s", "500ms", "2min", "1h", a bare number of seconds, or a clock
 * ("0:01:02.5", "01:02.5"). -1 if it is none of those.
 */
int64_t parseClock(const char* text) {
  if (!text) return -1;
  while (*text == ' ') ++text;
  if (strchr(text, ':')) {
    double parts[3] = {0, 0, 0};
    int count = 0;
    const char* p = text;
    while (count < 3) {
      char* end = nullptr;
      parts[count++] = strtod(p, &end);
      if (end == p) return -1;
      if (*end != ':') break;
      p = end + 1;
    }
    const double seconds = count == 3   ? parts[0] * 3600 + parts[1] * 60 + parts[2]
                           : count == 2 ? parts[0] * 60 + parts[1]
                                        : parts[0];
    return static_cast<int64_t>(seconds * 1000 + 0.5);
  }
  char* end = nullptr;
  const double value = strtod(text, &end);
  if (end == text) return -1;
  double scale = 1000;  // seconds by default
  if (strncmp(end, "ms", 2) == 0) {
    scale = 1;
  } else if (strncmp(end, "min", 3) == 0) {
    scale = 60000;
  } else if (*end == 'h') {
    scale = 3600000;
  }
  return static_cast<int64_t>(value * scale + 0.5);
}

/** Resolves an href found in `base` (an item path) to an item path, dropping any fragment. */
std::string resolve(const std::string& base, const std::string& href, std::string* fragment = nullptr) {
  std::string path = href;
  const size_t hash = path.find('#');
  if (fragment) *fragment = hash == std::string::npos ? std::string() : path.substr(hash + 1);
  if (hash != std::string::npos) path.resize(hash);
  const size_t slash = base.find_last_of('/');
  const std::string dir = slash == std::string::npos ? std::string() : base.substr(0, slash + 1);
  return FsHelpers::normalisePath(dir + FsHelpers::decodeUriPath(path));
}

/** Clips as parsed: audio by its index in `audioHrefs`, renumbered into tracks afterwards. */
struct RawClip {
  uint32_t idHash;
  uint32_t beginMs;
  uint16_t audio;
  uint16_t spine;
};

/** Streams one SMIL file through expat, appending its clips. */
class SmilParser final : public Print {
 public:
  SmilParser(std::string smilPath, const std::unordered_map<std::string, uint16_t>& spines,
             std::vector<std::string>& audioHrefs, std::vector<RawClip, EpubPsramAllocator<RawClip>>& out)
      : smilPath_(std::move(smilPath)), spines_(spines), audioHrefs_(audioHrefs), out_(out) {
    parser_ = XML_ParserCreate(nullptr);
    if (parser_) {
      XML_SetUserData(parser_, this);
      XML_SetElementHandler(parser_, startElement, endElement);
    }
  }
  ~SmilParser() override {
    if (parser_) XML_ParserFree(parser_);
  }

  size_t write(const uint8_t c) override { return write(&c, 1); }
  size_t write(const uint8_t* data, const size_t size) override {
    if (!parser_ || failed_) return 0;
    if (XML_Parse(parser_, reinterpret_cast<const char*>(data), static_cast<int>(size), XML_FALSE) ==
        XML_STATUS_ERROR) {
      failed_ = true;
      return 0;
    }
    return size;
  }
  bool finish() {
    if (!parser_ || failed_) return false;
    return XML_Parse(parser_, nullptr, 0, XML_TRUE) != XML_STATUS_ERROR;
  }

 private:
  static void XMLCALL startElement(void* userData, const XML_Char* name, const XML_Char** atts) {
    auto* self = static_cast<SmilParser*>(userData);
    const char* tag = localName(name);
    if (strcmp(tag, "par") == 0) {
      self->inPar_ = true;
      self->textPath_.clear();
      self->textId_.clear();
      self->audioPath_.clear();
      self->beginMs_ = -1;
    } else if (self->inPar_ && strcmp(tag, "text") == 0) {
      const char* src = attribute(atts, "src");
      if (src) self->textPath_ = resolve(self->smilPath_, src, &self->textId_);
    } else if (self->inPar_ && strcmp(tag, "audio") == 0) {
      const char* src = attribute(atts, "src");
      if (src) self->audioPath_ = resolve(self->smilPath_, src);
      self->beginMs_ = parseClock(attribute(atts, "clipBegin"));
      if (self->beginMs_ < 0) self->beginMs_ = 0;  // clipBegin defaults to the start of the file
    }
  }

  static void XMLCALL endElement(void* userData, const XML_Char* name) {
    auto* self = static_cast<SmilParser*>(userData);
    if (strcmp(localName(name), "par") != 0 || !self->inPar_) return;
    self->inPar_ = false;
    if (self->textId_.empty() || self->audioPath_.empty() || self->beginMs_ < 0) return;
    const auto spine = self->spines_.find(self->textPath_);
    if (spine == self->spines_.end()) return;
    const auto known = std::find(self->audioHrefs_.begin(), self->audioHrefs_.end(), self->audioPath_);
    const size_t audio = static_cast<size_t>(known - self->audioHrefs_.begin());
    if (known == self->audioHrefs_.end()) {
      if (self->audioHrefs_.size() >= UINT16_MAX) return;
      self->audioHrefs_.push_back(self->audioPath_);
    }
    self->out_.push_back(RawClip{Section::anchorHash(self->textId_.data(), self->textId_.size()),
                                 static_cast<uint32_t>(std::min<int64_t>(self->beginMs_, UINT32_MAX)),
                                 static_cast<uint16_t>(audio), spine->second});
  }

  XML_Parser parser_ = nullptr;
  bool failed_ = false;
  std::string smilPath_;
  const std::unordered_map<std::string, uint16_t>& spines_;
  std::vector<std::string>& audioHrefs_;
  std::vector<RawClip, EpubPsramAllocator<RawClip>>& out_;
  bool inPar_ = false;
  std::string textPath_;
  std::string textId_;
  std::string audioPath_;
  int64_t beginMs_ = -1;
};

}  // namespace

bool ReadAloudIndex::load(const Epub& epub, const std::function<void(int)>& progress) {
  error_.clear();
  tracks_.clear();
  clips_.clear();
  const std::string cache = epub.getCachePath() + CACHE_NAME;
  if (readCache(cache)) return true;
  tracks_.clear();
  clips_.clear();
  if (!build(epub, progress)) {
    tracks_.clear();
    clips_.clear();
    return false;
  }
  writeCache(cache);
  return true;
}

bool ReadAloudIndex::build(const Epub& epub, const std::function<void(int)>& progress) {
  const uint32_t startedMs = millis();
  const std::vector<std::string> smilFiles = epub.itemsWithSuffix(".smil");
  if (smilFiles.empty()) {
    error_ = "No read-aloud narration in this book";
    return false;
  }

  std::unordered_map<std::string, uint16_t> spines;
  const int spineCount = std::min(epub.getSpineItemsCount(), static_cast<int>(UINT16_MAX));
  for (int i = 0; i < spineCount; ++i) {
    spines.emplace(FsHelpers::normalisePath(FsHelpers::decodeUriPath(epub.getSpineItem(i).href)),
                   static_cast<uint16_t>(i));
  }

  std::vector<std::string> audioHrefs;
  std::vector<RawClip, EpubPsramAllocator<RawClip>> raw;
  for (size_t i = 0; i < smilFiles.size(); ++i) {
    if (progress) progress(static_cast<int>(i * 100 / smilFiles.size()));
    SmilParser parser(smilFiles[i], spines, audioHrefs, raw);
    if (!epub.readItemContentsToStream(smilFiles[i], parser, SMIL_CHUNK) || !parser.finish()) {
      INX_SERIAL.printf("[%lu] [RAL] Could not parse %s\n", millis(), smilFiles[i].c_str());
    }
    delay(1);  // let the idle tasks run (task watchdog)
  }
  if (raw.empty() || audioHrefs.empty()) {
    error_ = "The narration has no usable clips";
    return false;
  }
  if (raw.size() > UINT32_MAX / 2) {
    error_ = "The narration is too long";
    return false;
  }

  // Tracks in reading order: the order each audio file is first heard, walking the spine.
  std::stable_sort(raw.begin(), raw.end(), [](const RawClip& a, const RawClip& b) { return a.spine < b.spine; });
  std::vector<int> trackOf(audioHrefs.size(), -1);
  for (const RawClip& c : raw) {
    if (trackOf[c.audio] >= 0) continue;
    trackOf[c.audio] = static_cast<int>(tracks_.size());
    Track track;
    track.href = audioHrefs[c.audio].c_str();
    tracks_.push_back(std::move(track));
  }
  std::vector<std::string> hrefs;
  hrefs.reserve(tracks_.size());
  for (const Track& track : tracks_) hrefs.emplace_back(track.href.c_str());
  std::vector<std::pair<uint64_t, uint32_t>> ranges;
  if (!epub.getStoredItemRanges(hrefs, ranges)) {
    error_ = "Could not open the book file";
    return false;
  }
  for (size_t i = 0; i < tracks_.size(); ++i) {
    if (ranges[i].second == 0) {
      error_ = std::string("Narration audio is compressed or missing: ") + tracks_[i].href.c_str();
      return false;
    }
    tracks_[i].offset = ranges[i].first;
    tracks_[i].size = ranges[i].second;
  }

  clips_.reserve(raw.size());
  for (const RawClip& c : raw) {
    clips_.push_back(Clip{c.idHash, c.beginMs, static_cast<uint16_t>(trackOf[c.audio]), c.spine});
  }
  std::stable_sort(clips_.begin(), clips_.end(), [](const Clip& a, const Clip& b) {
    return a.track != b.track ? a.track < b.track : a.beginMs < b.beginMs;
  });
  if (progress) progress(100);
  INX_SERIAL.printf("[%lu] [RAL] %u clips, %u tracks from %u SMIL files in %lu ms\n", millis(),
                    static_cast<unsigned>(clips_.size()), static_cast<unsigned>(tracks_.size()),
                    static_cast<unsigned>(smilFiles.size()), static_cast<unsigned long>(millis() - startedMs));
  return true;
}

bool ReadAloudIndex::readCache(const std::string& path) {
  FsFile file;
  if (!SdMan.openFileForRead("RAL", path, file)) return false;
  uint32_t magic = 0;
  uint32_t version = 0;
  uint32_t trackCount = 0;
  serialization::readPod(file, magic);
  serialization::readPod(file, version);
  serialization::readPod(file, trackCount);
  bool ok = magic == CACHE_MAGIC && version == CACHE_VERSION && trackCount > 0 && trackCount <= UINT16_MAX;
  for (uint32_t i = 0; ok && i < trackCount; ++i) {
    Track track;
    std::string href;
    serialization::readString(file, href);
    track.href = href.c_str();
    serialization::readPod(file, track.offset);
    serialization::readPod(file, track.size);
    tracks_.push_back(std::move(track));
  }
  uint32_t clipCount = 0;
  if (ok) serialization::readPod(file, clipCount);
  ok = ok && clipCount > 0 && static_cast<uint64_t>(clipCount) * sizeof(Clip) <= file.available();
  if (ok) {
    clips_.resize(clipCount);
    const size_t bytes = clipCount * sizeof(Clip);
    ok = file.read(reinterpret_cast<uint8_t*>(clips_.data()), bytes) == static_cast<int>(bytes);
  }
  file.close();
  return ok;
}

void ReadAloudIndex::writeCache(const std::string& path) const {
  FsFile file;
  if (!SdMan.openFileForWrite("RAL", path, file)) return;
  serialization::writePod(file, CACHE_MAGIC);
  serialization::writePod(file, CACHE_VERSION);
  serialization::writePod(file, static_cast<uint32_t>(tracks_.size()));
  for (const Track& track : tracks_) {
    serialization::writeString(file, std::string(track.href.c_str()));
    serialization::writePod(file, track.offset);
    serialization::writePod(file, track.size);
  }
  serialization::writePod(file, static_cast<uint32_t>(clips_.size()));
  file.write(reinterpret_cast<const uint8_t*>(clips_.data()), clips_.size() * sizeof(Clip));
  file.close();
}

int ReadAloudIndex::clipAt(const int track, const uint32_t ms) const {
  // First clip after (track, ms), then step back one.
  const auto after = std::upper_bound(clips_.begin(), clips_.end(), std::make_pair(track, ms),
                                      [](const std::pair<int, uint32_t>& key, const Clip& c) {
                                        return key.first != c.track ? key.first < c.track : key.second < c.beginMs;
                                      });
  if (after == clips_.begin() || std::prev(after)->track != track) {
    // Before the first sentence of this track (a chapter's opening): its first clip, if it has one.
    return after != clips_.end() && after->track == track ? static_cast<int>(after - clips_.begin()) : -1;
  }
  return static_cast<int>(std::prev(after) - clips_.begin());
}

int ReadAloudIndex::firstClipOfSpine(const int spine) const {
  for (size_t i = 0; i < clips_.size(); ++i) {
    if (clips_[i].spine == spine) return static_cast<int>(i);
  }
  return -1;
}

#endif  // INX_HAS_AUDIO_MODULE
