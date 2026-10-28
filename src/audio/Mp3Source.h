#pragma once

/**
 * @file Mp3Source.h
 * @brief MP3 files: skips ID3 tags, finds the duration (Xing/Info/VBRI header or CBR estimate),
 * seeks through the Xing TOC when there is one, and passes the frames through untouched.
 */

#if INX_HAS_AUDIO_MODULE

#include <SDCardManager.h>

#include "AudioSource.h"

class Mp3Source final : public AudioSource {
 public:
  ~Mp3Source() override;

  bool open(const std::string& path) override;
  audiolink::Codec codec() const override { return audiolink::Codec::Mp3; }
  uint32_t durationMs() const override { return durationMs_; }
  uint32_t seek(uint32_t ms) override;
  size_t read(uint8_t* out, size_t max) override;
  bool atEnd() const override { return position_ >= dataEnd_; }

 private:
  struct FrameHeader {
    bool valid = false;
    bool mpeg1 = false;
    bool mono = false;
    uint32_t sampleRate = 0;
    uint32_t bitrateKbps = 0;
    uint32_t frameBytes = 0;
    uint32_t samplesPerFrame = 0;
  };

  static FrameHeader parseHeader(const uint8_t* bytes);
  /** First offset >= from where two consecutive frame headers check out; UINT32_MAX if none. */
  uint32_t findFrame(uint32_t from, uint32_t limit);
  void readVbrHeader(const FrameHeader& first);

  FsFile file_;
  uint32_t dataStart_ = 0;  ///< first frame
  uint32_t dataEnd_ = 0;    ///< before an ID3v1 tag
  uint32_t position_ = 0;
  uint32_t durationMs_ = 0;
  uint32_t sampleRate_ = 0;
  uint32_t vbrFrames_ = 0;
  uint32_t vbrBytes_ = 0;
  bool hasToc_ = false;
  uint8_t toc_[100] = {};
};

#endif  // INX_HAS_AUDIO_MODULE
