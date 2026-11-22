/**
 * @file AudioSource.cpp
 * @brief Reader selection by file extension.
 */

#include "AudioSource.h"

#if INX_HAS_AUDIO_MODULE

#include <Arduino.h>
#include <SDCardManager.h>

#include <algorithm>
#include <cctype>

#include "Mp3Source.h"
#include "Mp4Source.h"

namespace {

std::string extension(const std::string& name) {
  const size_t dot = name.find_last_of('.');
  if (dot == std::string::npos) return "";
  std::string ext = name.substr(dot + 1);
  std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
  return ext;
}

}  // namespace

bool AudioSource::isAudioFile(const std::string& name) {
  const std::string ext = extension(name);
  return ext == "mp3" || ext == "m4b" || ext == "m4a" || ext == "mp4";
}

std::unique_ptr<AudioSource> AudioSource::forName(const std::string& name) {
  const std::string ext = extension(name);
  if (ext == "mp3") return std::make_unique<Mp3Source>();
  if (ext == "m4b" || ext == "m4a" || ext == "mp4") return std::make_unique<Mp4Source>();
  return nullptr;
}

namespace {

class SdAudioFile final : public AudioFile {
 public:
  explicit SdAudioFile(FsFile file) : file_(std::move(file)) {}
  ~SdAudioFile() override { file_.close(); }
  bool seekSet(const uint64_t position) override { return file_.seekSet(position); }
  int read(void* buffer, const size_t length) override { return file_.read(buffer, length); }
  uint64_t fileSize() override { return file_.fileSize(); }

 private:
  FsFile file_;
};

/** A window of a larger file, read as if it were the whole file. */
class SdRangeAudioFile final : public AudioFile {
 public:
  SdRangeAudioFile(FsFile file, const uint64_t offset, const uint64_t length)
      : file_(std::move(file)), offset_(offset), length_(length) {}
  ~SdRangeAudioFile() override { file_.close(); }
  bool seekSet(const uint64_t position) override {
    if (position > length_) return false;
    position_ = position;
    return file_.seekSet(offset_ + position);
  }
  int read(void* buffer, const size_t length) override {
    const uint64_t left = length_ - position_;
    const size_t want = static_cast<size_t>(std::min<uint64_t>(length, left));
    if (want == 0) return 0;
    const int got = file_.read(buffer, want);
    if (got > 0) position_ += static_cast<uint64_t>(got);
    return got;
  }
  uint64_t fileSize() override { return length_; }

 private:
  FsFile file_;
  uint64_t offset_;
  uint64_t length_;
  uint64_t position_ = 0;
};

}  // namespace

std::unique_ptr<AudioFile> AudioFile::openSd(const std::string& path) {
  FsFile file = SdMan.open(path.c_str(), O_RDONLY);
  if (!file) return nullptr;
  return std::make_unique<SdAudioFile>(std::move(file));
}

std::unique_ptr<AudioFile> AudioFile::openSdRange(const std::string& path, const uint64_t offset,
                                                  const uint64_t length) {
  FsFile file = SdMan.open(path.c_str(), O_RDONLY);
  if (!file) return nullptr;
  if (offset + length > file.fileSize()) {
    file.close();
    return nullptr;
  }
  // The window is usually deep inside a large file. On FAT32 every backward seek (the MP4 reader
  // makes many) walks the cluster chain from the start; once SdFat knows the file is unfragmented
  // it seeks in constant time. One walk of the chain, here.
  const uint32_t startedMs = millis();
  const bool contiguous = file.contiguousRange(nullptr, nullptr);
  INX_SERIAL.printf("[%lu] [AUD] %s: %s, chain checked in %lu ms\n", millis(), path.c_str(),
                    contiguous ? "contiguous" : "fragmented", static_cast<unsigned long>(millis() - startedMs));
  if (!file.seekSet(offset)) {
    file.close();
    return nullptr;
  }
  return std::make_unique<SdRangeAudioFile>(std::move(file), offset, length);
}

#endif  // INX_HAS_AUDIO_MODULE
