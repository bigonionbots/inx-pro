#pragma once

/**
 * @file AudioFile.h
 * @brief Random-access byte source for the audio readers: a file on the SD card or a remote file
 * read through HTTP range requests (HttpAudioFile.h).
 */

#if INX_HAS_AUDIO_MODULE

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

class AudioFile {
 public:
  virtual ~AudioFile() = default;
  virtual bool seekSet(uint64_t position) = 0;
  /** Bytes read; 0 at the end of the file, negative on an error that retrying did not fix. */
  virtual int read(void* buffer, size_t length) = 0;
  virtual uint64_t fileSize() = 0;

  /** Opens a file on the SD card; nullptr if it cannot be opened. The caller holds SdIoMutex. */
  static std::unique_ptr<AudioFile> openSd(const std::string& path);
};

#endif  // INX_HAS_AUDIO_MODULE
