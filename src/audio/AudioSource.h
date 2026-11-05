#pragma once

/**
 * @file AudioSource.h
 * @brief One audio file turned into the codec byte stream the module decodes.
 *
 * Sources read through an AudioFile. For files on the SD card that happens only on the main loop
 * task (AudioPlayer::service()) with SdIoMutex held; for remote files (HttpAudioFile) on the
 * player's network feeder task.
 */

#if INX_HAS_AUDIO_MODULE

#include <AudioLinkProtocol.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "AudioChapter.h"
#include "AudioFile.h"

class AudioSource {
 public:
  virtual ~AudioSource() = default;

  virtual bool open(std::unique_ptr<AudioFile> file) = 0;
  virtual audiolink::Codec codec() const = 0;
  /** 0 when unknown. */
  virtual uint32_t durationMs() const = 0;
  /** Positions the stream at or just before ms; returns the position actually reached. */
  virtual uint32_t seek(uint32_t ms) = 0;
  /** Up to max bytes of the codec stream; 0 at the end of the file or on a read error. */
  virtual size_t read(uint8_t* out, size_t max) = 0;
  virtual bool atEnd() const = 0;
  /** Chapters stored in the file (times from its start), in order; empty when it has none. */
  virtual const std::vector<AudioChapter>& chapters() const {
    static const std::vector<AudioChapter> none;
    return none;
  }
  /** Where cover art embedded in the file is (offset and length in the file), if it has any. */
  virtual bool embeddedCover(uint64_t& offset, uint32_t& length, bool& png) const {
    (void)offset;
    (void)length;
    (void)png;
    return false;
  }
  /** Why open() failed, for the player's message line. */
  const std::string& error() const { return error_; }

  /** Picks the reader from the file name's extension; nullptr for files it cannot play. */
  static std::unique_ptr<AudioSource> forName(const std::string& name);
  static bool isAudioFile(const std::string& name);

 protected:
  std::string error_;
};

#endif  // INX_HAS_AUDIO_MODULE
