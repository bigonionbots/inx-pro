#pragma once

/**
 * @file AudioSource.h
 * @brief One audio file turned into the codec byte stream the module decodes.
 *
 * Sources only touch the SD card from the main loop task (AudioPlayer::service()), and the caller
 * holds SdIoMutex around every call.
 */

#if INX_HAS_AUDIO_MODULE

#include <AudioLinkProtocol.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

class AudioSource {
 public:
  virtual ~AudioSource() = default;

  virtual bool open(const std::string& path) = 0;
  virtual audiolink::Codec codec() const = 0;
  /** 0 when unknown. */
  virtual uint32_t durationMs() const = 0;
  /** Positions the stream at or just before ms; returns the position actually reached. */
  virtual uint32_t seek(uint32_t ms) = 0;
  /** Up to max bytes of the codec stream; 0 at the end of the file or on a read error. */
  virtual size_t read(uint8_t* out, size_t max) = 0;
  virtual bool atEnd() const = 0;
  /** Why open() failed, for the player's message line. */
  const std::string& error() const { return error_; }

  /** Picks the reader from the file extension; nullptr for files it cannot play. */
  static std::unique_ptr<AudioSource> forPath(const std::string& path);
  static bool isAudioFile(const std::string& name);

 protected:
  std::string error_;
};

#endif  // INX_HAS_AUDIO_MODULE
