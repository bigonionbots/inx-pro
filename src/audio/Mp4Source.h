#pragma once

/**
 * @file Mp4Source.h
 * @brief AAC in MP4 containers (.m4b / .m4a / .mp4): demuxes the first audio track to ADTS.
 *
 * Only the small tables (stts, stsc) are loaded. Sample sizes (stsz) and chunk offsets (stco/co64)
 * are read from the card on demand through small caches, because a long audiobook has millions of
 * frames. HE-AAC (SBR/PS) is sent as its AAC-LC core in the ADTS header; the module's decoder picks
 * up the SBR data from the bitstream.
 */

#if INX_HAS_AUDIO_MODULE

#include <SDCardManager.h>

#include <vector>

#include "AudioSource.h"

class Mp4Source final : public AudioSource {
 public:
  ~Mp4Source() override;

  bool open(const std::string& path) override;
  audiolink::Codec codec() const override { return audiolink::Codec::AacAdts; }
  uint32_t durationMs() const override { return durationMs_; }
  uint32_t seek(uint32_t ms) override;
  size_t read(uint8_t* out, size_t max) override;
  bool atEnd() const override { return sample_ >= sampleCount_ && pendingSize_ == pendingRead_; }

  /** ©nam from the iTunes metadata, empty if absent. */
  const std::string& title() const { return title_; }

 private:
  struct Box {
    uint64_t start = 0;    ///< first byte of the box header
    uint64_t payload = 0;  ///< first byte after the header
    uint64_t end = 0;
    char type[5] = {};
  };
  struct SttsEntry {
    uint32_t count;
    uint32_t delta;
  };
  struct StscEntry {
    uint32_t firstChunk;  ///< 1-based, as stored
    uint32_t samplesPerChunk;
  };

  bool readBox(uint64_t at, uint64_t limit, Box& box);
  bool findChild(uint64_t from, uint64_t end, const char* type, Box& out);
  bool readU32(uint64_t at, uint32_t& value);
  bool parseTrack(const Box& trak);
  bool parseSampleEntry(const Box& stsd);
  bool parseEsds(const Box& esds);
  bool parseAudioSpecificConfig(const uint8_t* data, size_t length);
  void parseTitle(const Box& moov);

  uint32_t sampleSize(uint32_t sample);
  uint64_t chunkOffset(uint32_t chunk);
  void positionAt(uint32_t sample);
  uint64_t sampleTimeUnits(uint32_t sample) const;
  uint32_t sampleAtTime(uint64_t units) const;

  FsFile file_;
  uint64_t fileSize_ = 0;
  std::string title_;

  // Track
  uint32_t timescale_ = 0;
  uint32_t durationMs_ = 0;
  uint8_t adtsProfile_ = 1;  ///< AAC object type - 1 (LC = 1)
  uint8_t sampleRateIndex_ = 4;
  uint8_t channelConfig_ = 2;
  bool haveConfig_ = false;

  // Tables
  std::vector<SttsEntry> stts_;
  std::vector<StscEntry> stsc_;
  uint32_t fixedSampleSize_ = 0;
  uint32_t sampleCount_ = 0;
  uint64_t stszTable_ = 0;
  uint32_t chunkCount_ = 0;
  uint64_t chunkTable_ = 0;
  bool chunkOffsets64_ = false;

  // Small read caches for the on-card tables
  static constexpr uint32_t CACHE_ENTRIES = 256;
  uint32_t sizeCache_[CACHE_ENTRIES] = {};
  uint32_t sizeCacheFirst_ = UINT32_MAX;
  uint64_t offsetCache_[CACHE_ENTRIES] = {};
  uint32_t offsetCacheFirst_ = UINT32_MAX;

  // Cursor
  uint32_t sample_ = 0;
  uint32_t chunk_ = 0;  ///< 0-based
  uint32_t sampleInChunk_ = 0;
  size_t stscIndex_ = 0;
  uint64_t offset_ = 0;

  // One ADTS frame being handed out
  std::vector<uint8_t> pending_;
  size_t pendingSize_ = 0;
  size_t pendingRead_ = 0;
};

#endif  // INX_HAS_AUDIO_MODULE
