/**
 * @file AudioSource.cpp
 * @brief Reader selection by file extension.
 */

#include "AudioSource.h"

#if INX_HAS_AUDIO_MODULE

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

std::unique_ptr<AudioSource> AudioSource::forPath(const std::string& path) {
  const std::string ext = extension(path);
  if (ext == "mp3") return std::make_unique<Mp3Source>();
  if (ext == "m4b" || ext == "m4a" || ext == "mp4") return std::make_unique<Mp4Source>();
  return nullptr;
}

#endif  // INX_HAS_AUDIO_MODULE
