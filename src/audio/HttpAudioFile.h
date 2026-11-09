#pragma once

/**
 * @file HttpAudioFile.h
 * @brief AudioFile over HTTP range requests, for streaming from Audiobookshelf.
 *
 * Reads are served from a few 128 KB blocks cached in PSRAM. Three blocks cover the access
 * pattern of the readers: the audio data being played plus, for MP4, the sample-size and
 * chunk-offset tables elsewhere in the file.
 *
 * Playback reads straight through the file, so the blocks after one another come from a single
 * open-ended request (`Range: bytes=N-`) kept open between misses: a fresh HTTPS connection per
 * block cost a TLS handshake each, about a second through a Cloudflare tunnel, which held
 * streaming to ~50 KB/s and made playback stop to buffer. Any other miss (the MP4 tables, the
 * first block after a seek) closes the stream and is one `Range: bytes=a-b` request; the next
 * block in sequence reopens it, as it does after a dropped stream. One open connection at most:
 * each holds memory.
 *
 * A 401 renews the Audiobookshelf access token once; network errors are retried with backoff
 * before a read fails. Blocking: use it from a network task, never the main loop.
 */

#if INX_HAS_AUDIO_MODULE

#include <atomic>
#include <string>

#include "AudioFile.h"

struct esp_http_client;

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
  /** Opens the sequential stream at `start` (closing any other). False: use range requests. */
  bool openStream(uint64_t start);
  /** Fills `into` from the stream, which must be at its start; closes the stream on a short read. */
  bool readStream(Block& into, size_t length);
  void closeStream();

  std::string url_;
  uint64_t size_ = 0;
  uint64_t position_ = 0;
  uint32_t useCounter_ = 0;
  Block blocks_[BLOCKS];
  esp_http_client* stream_ = nullptr;  ///< the open sequential request, if any
  uint64_t streamPos_ = 0;             ///< file offset its next byte is at
  uint64_t lastMiss_ = UINT64_MAX;     ///< the block the last miss was for
  uint64_t streamSize_ = 0;            ///< the stream's Content-Range total, while opening
  bool streamUnsupported_ = false;     ///< the server ignores open-ended ranges
  std::atomic<bool> cancelled_{false};
  bool failed_ = false;
  std::string lastError_;
};

#endif  // INX_HAS_AUDIO_MODULE
