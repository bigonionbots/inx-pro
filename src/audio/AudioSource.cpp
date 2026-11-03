/**
 * @file AudioSource.cpp
 * @brief Reader selection by file extension.
 */

#include "AudioSource.h"

#if INX_HAS_AUDIO_MODULE

#include <algorithm>
#include <cctype>

#include <SDCardManager.h>

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

}  // namespace

std::unique_ptr<AudioFile> AudioFile::openSd(const std::string& path) {
  FsFile file = SdMan.open(path.c_str(), O_RDONLY);
  if (!file) return nullptr;
  return std::make_unique<SdAudioFile>(std::move(file));
}

#endif  // INX_HAS_AUDIO_MODULE
