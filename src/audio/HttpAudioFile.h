#pragma once

/**
 * @file HttpAudioFile.h
 * @brief AudioFile over HTTP range requests, for streaming from Audiobookshelf.
 *
 * Reads are served from a few 128 KB blocks cached in PSRAM; a miss fetches the block with
 * `Range: bytes=a-b`. Three blocks cover the access pattern of the readers: the audio data being
 * played plus, for MP4, the sample-size and chunk-offset tables elsewhere in the file. A 401
 * renews the Audiobookshelf access token once; network errors are retried with backoff before a
 * read fails. Blocking: use it from a network task, never the main loop.
 */

#if INX_HAS_AUDIO_MODULE

#include <atomic>
#include <string>

#include "AudioFile.h"

class HttpAudioFile final : public AudioFile {
 public:
  explicit HttpAudioFile(std::string url);
  ~HttpAudioFile() override;

  /** Fetches the first block, which also yields the file size. */
  bool open(std::string& error);
  /** Makes reads give up quickly (the player stopping or seeking). Any task. */
  void cancel() { cancelled_ = true; }
  /** A read gave up on a network error (as opposed to reaching the end of the file). */
  bool failed() const { return failed_; }
  const std::string& lastError() const { return lastError_; }

  bool seekSet(uint64_t position) override;
  int read(void* buffer, size_t length) override;
  uint64_t fileSize() override { return size_; }

  static constexpr size_t BLOCK_BYTES = 128 * 1024;
  static constexpr int BLOCKS = 3;

 private:
  struct Block {
    uint64_t index = UINT64_MAX;
    size_t length = 0;
    uint32_t lastUse = 0;
    uint8_t* data = nullptr;
  };

  Block* block(uint64_t index);
  bool fetch(uint64_t index, Block& into);
  bool fetchOnce(uint64_t start, size_t length, Block& into, int& status);

  std::string url_;
  uint64_t size_ = 0;
  uint64_t position_ = 0;
  uint32_t useCounter_ = 0;
  Block blocks_[BLOCKS];
  std::atomic<bool> cancelled_{false};
  bool failed_ = false;
  std::string lastError_;
};

#endif  // INX_HAS_AUDIO_MODULE
