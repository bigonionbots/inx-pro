/**
 * @file Mp3Source.cpp
 * @brief Definitions for Mp3Source.
 */

#include "Mp3Source.h"

#if INX_HAS_AUDIO_MODULE

#include <cstring>

namespace {

constexpr uint32_t SYNC_SEARCH_BYTES = 128 * 1024;
constexpr size_t SCAN_WINDOW = 4096;
constexpr uint32_t NOT_FOUND = UINT32_MAX;

constexpr uint16_t BITRATE_MPEG1[16] = {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0};
constexpr uint16_t BITRATE_MPEG2[16] = {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0};
constexpr uint32_t SAMPLE_RATE_MPEG1[3] = {44100, 48000, 32000};

uint32_t be32(const uint8_t* p) {
  return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) | p[3];
}

}  // namespace

Mp3Source::~Mp3Source() {
  if (file_) file_.close();
}

Mp3Source::FrameHeader Mp3Source::parseHeader(const uint8_t* b) {
  FrameHeader h;
  if (b[0] != 0xFF || (b[1] & 0xE0) != 0xE0) return h;
  const uint8_t version = (b[1] >> 3) & 3;  // 0: 2.5, 1: reserved, 2: 2, 3: 1
  const uint8_t layer = (b[1] >> 1) & 3;    // 1: layer III
  const uint8_t bitrateIndex = (b[2] >> 4) & 0xF;
  const uint8_t rateIndex = (b[2] >> 2) & 3;
  if (version == 1 || layer != 1 || bitrateIndex == 0 || bitrateIndex == 15 || rateIndex == 3) return h;
  h.mpeg1 = version == 3;
  h.mono = ((b[3] >> 6) & 3) == 3;
  h.sampleRate = SAMPLE_RATE_MPEG1[rateIndex] >> (h.mpeg1 ? 0 : (version == 2 ? 1 : 2));
  h.bitrateKbps = h.mpeg1 ? BITRATE_MPEG1[bitrateIndex] : BITRATE_MPEG2[bitrateIndex];
  h.samplesPerFrame = h.mpeg1 ? 1152 : 576;
  const uint32_t padding = (b[2] >> 1) & 1;
  h.frameBytes = (h.samplesPerFrame / 8) * h.bitrateKbps * 1000 / h.sampleRate + padding;
  h.valid = h.frameBytes > 4;
  return h;
}

uint32_t Mp3Source::findFrame(uint32_t from, const uint32_t limit) {
  uint8_t window[SCAN_WINDOW];
  const uint32_t stop = std::min(dataEnd_, from + limit);
  while (from + 4 < stop) {
    file_.seekSet(from);
    const int got = file_.read(window, sizeof(window));
    if (got < 8) return 0;
    for (int i = 0; i + 4 <= got; ++i) {
      const FrameHeader h = parseHeader(window + i);
      if (!h.valid || (sampleRate_ && h.sampleRate != sampleRate_)) continue;
      // Confirm with the next header, so a stray 0xFFE in the data is not taken for a frame.
      uint8_t next[4];
      const uint32_t nextOffset = from + i + h.frameBytes;
      if (nextOffset + 4 > dataEnd_) return from + i;
      file_.seekSet(nextOffset);
      if (file_.read(next, 4) == 4) {
        const FrameHeader n = parseHeader(next);
        if (n.valid && n.sampleRate == h.sampleRate) return from + i;
      }
    }
    from += static_cast<uint32_t>(got) - 3;
  }
  return NOT_FOUND;
}

void Mp3Source::readVbrHeader(const FrameHeader& first) {
  uint8_t frame[200];
  file_.seekSet(dataStart_);
  const int got = file_.read(frame, sizeof(frame));
  const uint32_t sideInfo = first.mpeg1 ? (first.mono ? 17 : 32) : (first.mono ? 9 : 17);
  const uint32_t xing = 4 + sideInfo;
  if (got >= static_cast<int>(xing + 8) &&
      (std::memcmp(frame + xing, "Xing", 4) == 0 || std::memcmp(frame + xing, "Info", 4) == 0)) {
    const uint32_t flags = be32(frame + xing + 4);
    uint32_t at = xing + 8;
    if ((flags & 1) && got >= static_cast<int>(at + 4)) {
      vbrFrames_ = be32(frame + at);
      at += 4;
    }
    if ((flags & 2) && got >= static_cast<int>(at + 4)) {
      vbrBytes_ = be32(frame + at);
      at += 4;
    }
    if ((flags & 4) && got >= static_cast<int>(at + 100)) {
      std::memcpy(toc_, frame + at, sizeof(toc_));
      hasToc_ = true;
    }
    // The Xing frame is silent padding; start playback after it.
    dataStart_ += first.frameBytes;
    return;
  }
  if (got >= 4 + 32 + 18 && std::memcmp(frame + 36, "VBRI", 4) == 0) {
    vbrBytes_ = be32(frame + 36 + 10);
    vbrFrames_ = be32(frame + 36 + 14);
    dataStart_ += first.frameBytes;
  }
}

bool Mp3Source::open(const std::string& path) {
  file_ = SdMan.open(path.c_str(), O_RDONLY);
  if (!file_) {
    error_ = "cannot open file";
    return false;
  }
  dataEnd_ = static_cast<uint32_t>(file_.fileSize());

  uint8_t head[10];
  if (file_.read(head, sizeof(head)) == sizeof(head) && std::memcmp(head, "ID3", 3) == 0) {
    const uint32_t size = (static_cast<uint32_t>(head[6] & 0x7F) << 21) | ((head[7] & 0x7F) << 14) |
                          ((head[8] & 0x7F) << 7) | (head[9] & 0x7F);
    dataStart_ = 10 + size + ((head[5] & 0x10) ? 10 : 0);
  }
  if (dataEnd_ > 128) {
    uint8_t tag[3];
    file_.seekSet(dataEnd_ - 128);
    if (file_.read(tag, 3) == 3 && std::memcmp(tag, "TAG", 3) == 0) dataEnd_ -= 128;
  }

  const uint32_t first = findFrame(dataStart_, SYNC_SEARCH_BYTES);
  if (first == NOT_FOUND) {
    error_ = "no MP3 frames found";
    return false;
  }
  uint8_t bytes[4] = {};
  file_.seekSet(first);
  file_.read(bytes, 4);
  const FrameHeader header = parseHeader(bytes);
  if (!header.valid) {
    error_ = "not an MP3 (layer III) file";
    return false;
  }
  dataStart_ = first;
  sampleRate_ = header.sampleRate;
  readVbrHeader(header);

  if (vbrFrames_ > 0) {
    durationMs_ = static_cast<uint32_t>(static_cast<uint64_t>(vbrFrames_) * header.samplesPerFrame * 1000 /
                                        header.sampleRate);
  } else {
    durationMs_ = static_cast<uint32_t>(static_cast<uint64_t>(dataEnd_ - dataStart_) * 8 / header.bitrateKbps);
  }
  position_ = dataStart_;
  file_.seekSet(position_);
  return true;
}

uint32_t Mp3Source::seek(const uint32_t ms) {
  if (ms == 0 || durationMs_ == 0) {
    position_ = dataStart_;
    file_.seekSet(position_);
    return 0;
  }
  const uint32_t target = std::min(ms, durationMs_);
  const double fraction = static_cast<double>(target) / durationMs_;
  const uint32_t span = dataEnd_ - dataStart_;
  uint32_t offset;
  if (hasToc_) {
    const double percent = fraction * 100.0;
    const int index = std::min(99, static_cast<int>(percent));
    const double a = toc_[index];
    const double b = index < 99 ? toc_[index + 1] : 256.0;
    const double tocValue = a + (b - a) * (percent - index);
    const uint32_t bytes = vbrBytes_ ? vbrBytes_ : span;
    offset = dataStart_ + static_cast<uint32_t>(tocValue / 256.0 * bytes);
  } else {
    offset = dataStart_ + static_cast<uint32_t>(fraction * span);
  }
  const uint32_t frame = findFrame(std::min(offset, dataEnd_), 64 * 1024);
  position_ = frame == NOT_FOUND ? dataEnd_ : frame;
  file_.seekSet(position_);
  return target;
}

size_t Mp3Source::read(uint8_t* out, const size_t max) {
  if (position_ >= dataEnd_) return 0;
  const size_t want = std::min<size_t>(max, dataEnd_ - position_);
  file_.seekSet(position_);
  const int got = file_.read(out, want);
  if (got <= 0) {
    position_ = dataEnd_;
    return 0;
  }
  position_ += static_cast<uint32_t>(got);
  return static_cast<size_t>(got);
}

#endif  // INX_HAS_AUDIO_MODULE
