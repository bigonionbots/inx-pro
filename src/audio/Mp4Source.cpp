/**
 * @file Mp4Source.cpp
 * @brief Definitions for Mp4Source.
 */

#include "Mp4Source.h"

#if INX_HAS_AUDIO_MODULE

#include <algorithm>
#include <cstring>

namespace {

constexpr size_t MAX_TABLE_ENTRIES = 200000;  // stts / stsc entries kept in RAM (PSRAM via std::vector)
constexpr uint32_t MAX_FRAME_BYTES = 8192;
constexpr size_t ADTS_HEADER = 7;
constexpr uint32_t SAMPLE_RATES[13] = {96000, 88200, 64000, 48000, 44100, 32000, 24000,
                                       22050, 16000, 12000, 11025, 8000,  7350};

uint32_t be32(const uint8_t* p) {
  return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) | p[3];
}

uint64_t be64(const uint8_t* p) { return (static_cast<uint64_t>(be32(p)) << 32) | be32(p + 4); }

class BitReader {
 public:
  BitReader(const uint8_t* data, const size_t length) : data_(data), bits_(length * 8) {}
  uint32_t read(int count) {
    uint32_t value = 0;
    while (count-- > 0) {
      value <<= 1;
      if (position_ < bits_) value |= (data_[position_ / 8] >> (7 - position_ % 8)) & 1;
      ++position_;
    }
    return value;
  }
  bool overrun() const { return position_ > bits_; }

 private:
  const uint8_t* data_;
  size_t bits_;
  size_t position_ = 0;
};

uint32_t readObjectType(BitReader& bits) {
  const uint32_t type = bits.read(5);
  return type == 31 ? 32 + bits.read(6) : type;
}

uint32_t readSampleRateIndex(BitReader& bits) {
  const uint32_t index = bits.read(4);
  if (index == 15) bits.read(24);  // explicit frequency: not representable in ADTS
  return index;
}

}  // namespace

Mp4Source::~Mp4Source() {
  if (file_) file_.close();
}

bool Mp4Source::readU32(const uint64_t at, uint32_t& value) {
  uint8_t bytes[4];
  if (!file_.seekSet(at) || file_.read(bytes, 4) != 4) return false;
  value = be32(bytes);
  return true;
}

bool Mp4Source::readBox(const uint64_t at, const uint64_t limit, Box& box) {
  uint8_t header[16];
  if (at + 8 > limit || !file_.seekSet(at) || file_.read(header, 8) != 8) return false;
  uint64_t size = be32(header);
  std::memcpy(box.type, header + 4, 4);
  box.type[4] = '\0';
  box.start = at;
  box.payload = at + 8;
  if (size == 1) {
    if (file_.read(header + 8, 8) != 8) return false;
    size = be64(header + 8);
    box.payload = at + 16;
  } else if (size == 0) {
    size = limit - at;
  }
  if (size < box.payload - at || at + size > limit) return false;
  box.end = at + size;
  return true;
}

bool Mp4Source::findChild(uint64_t from, const uint64_t end, const char* type, Box& out) {
  Box box;
  while (readBox(from, end, box)) {
    if (std::memcmp(box.type, type, 4) == 0) {
      out = box;
      return true;
    }
    from = box.end;
  }
  return false;
}

bool Mp4Source::parseAudioSpecificConfig(const uint8_t* data, const size_t length) {
  BitReader bits(data, length);
  uint32_t objectType = readObjectType(bits);
  uint32_t rateIndex = readSampleRateIndex(bits);
  const uint32_t channels = bits.read(4);
  if (objectType == 5 || objectType == 29) {
    // HE-AAC / HE-AACv2: an extension rate and the core object type follow. ADTS carries the core.
    readSampleRateIndex(bits);
    objectType = readObjectType(bits);
  }
  if (bits.overrun() || objectType < 1 || objectType > 4 || rateIndex > 12) {
    error_ = "unsupported AAC configuration";
    return false;
  }
  if (channels == 0 || channels > 7) {
    error_ = "unsupported AAC channel layout";
    return false;
  }
  adtsProfile_ = static_cast<uint8_t>(objectType - 1);
  sampleRateIndex_ = static_cast<uint8_t>(rateIndex);
  channelConfig_ = static_cast<uint8_t>(channels);
  haveConfig_ = true;
  return true;
}

bool Mp4Source::parseEsds(const Box& esds) {
  const size_t length = static_cast<size_t>(std::min<uint64_t>(esds.end - esds.payload, 256));
  uint8_t data[256];
  if (!file_.seekSet(esds.payload) || file_.read(data, length) != static_cast<int>(length)) return false;
  size_t at = 4;  // FullBox version + flags
  auto descriptor = [&](uint8_t& tag, size_t& size) {
    if (at >= length) return false;
    tag = data[at++];
    size = 0;
    for (int i = 0; i < 4 && at < length; ++i) {
      const uint8_t b = data[at++];
      size = (size << 7) | (b & 0x7F);
      if (!(b & 0x80)) break;
    }
    return true;
  };
  uint8_t tag;
  size_t size;
  if (!descriptor(tag, size) || tag != 0x03) return false;
  at += 2;  // ES_ID
  const uint8_t flags = data[at++];
  if (flags & 0x80) at += 2;
  if ((flags & 0x40) && at < length) at += 1 + data[at];
  if (flags & 0x20) at += 2;
  if (!descriptor(tag, size) || tag != 0x04 || at + 13 > length) return false;
  const uint8_t objectTypeIndication = data[at];
  if (objectTypeIndication != 0x40 && objectTypeIndication != 0x66 && objectTypeIndication != 0x67) {
    error_ = "audio track is not AAC";
    return false;
  }
  at += 13;
  if (!descriptor(tag, size) || tag != 0x05 || at + size > length) return false;
  return parseAudioSpecificConfig(data + at, size);
}

bool Mp4Source::parseSampleEntry(const Box& stsd) {
  Box entry;
  if (!readBox(stsd.payload + 8, stsd.end, entry)) return false;  // FullBox + entry_count
  if (std::memcmp(entry.type, "mp4a", 4) != 0) {
    error_ = std::string("unsupported codec ") + entry.type;
    return false;
  }
  // AudioSampleEntry: 28 bytes of fields; QuickTime sound versions 1 and 2 add 16 or 36.
  uint8_t fields[28];
  if (!file_.seekSet(entry.payload) || file_.read(fields, sizeof(fields)) != sizeof(fields)) return false;
  const uint16_t version = static_cast<uint16_t>((fields[8] << 8) | fields[9]);
  const uint64_t children = entry.payload + 28 + (version == 1 ? 16 : (version == 2 ? 36 : 0));
  Box esds;
  if (findChild(children, entry.end, "esds", esds)) return parseEsds(esds);
  Box wave;
  if (findChild(children, entry.end, "wave", wave) && findChild(wave.payload, wave.end, "esds", esds)) {
    return parseEsds(esds);
  }
  error_ = "no AAC configuration (esds)";
  return false;
}

bool Mp4Source::parseTrack(const Box& trak) {
  Box mdia, hdlr, mdhd, minf, stbl;
  if (!findChild(trak.payload, trak.end, "mdia", mdia)) return false;
  if (!findChild(mdia.payload, mdia.end, "hdlr", hdlr)) return false;
  uint8_t handler[12];
  if (!file_.seekSet(hdlr.payload) || file_.read(handler, 12) != 12 || std::memcmp(handler + 8, "soun", 4) != 0) {
    return false;
  }
  if (!findChild(mdia.payload, mdia.end, "mdhd", mdhd)) return false;
  uint8_t header[32];
  if (!file_.seekSet(mdhd.payload) || file_.read(header, 32) != 32) return false;
  uint64_t duration;
  if (header[0] == 1) {
    timescale_ = be32(header + 20);
    duration = be64(header + 24);
  } else {
    timescale_ = be32(header + 12);
    duration = be32(header + 16);
  }
  if (timescale_ == 0) return false;
  durationMs_ = static_cast<uint32_t>(duration * 1000 / timescale_);

  if (!findChild(mdia.payload, mdia.end, "minf", minf) || !findChild(minf.payload, minf.end, "stbl", stbl)) {
    return false;
  }
  Box stsd, stts, stsc, stsz, stco;
  if (!findChild(stbl.payload, stbl.end, "stsd", stsd) || !parseSampleEntry(stsd)) return false;

  uint32_t count = 0;
  if (!findChild(stbl.payload, stbl.end, "stts", stts) || !readU32(stts.payload + 4, count)) return false;
  stts_.resize(std::min<size_t>(count, MAX_TABLE_ENTRIES));
  file_.seekSet(stts.payload + 8);
  for (SttsEntry& e : stts_) {
    uint8_t b[8];
    if (file_.read(b, 8) != 8) return false;
    e = {be32(b), be32(b + 4)};
  }

  if (!findChild(stbl.payload, stbl.end, "stsc", stsc) || !readU32(stsc.payload + 4, count)) return false;
  stsc_.resize(std::min<size_t>(count, MAX_TABLE_ENTRIES));
  file_.seekSet(stsc.payload + 8);
  for (StscEntry& e : stsc_) {
    uint8_t b[12];
    if (file_.read(b, 12) != 12) return false;
    e = {be32(b), be32(b + 4)};
  }

  if (!findChild(stbl.payload, stbl.end, "stsz", stsz) || !readU32(stsz.payload + 4, fixedSampleSize_) ||
      !readU32(stsz.payload + 8, sampleCount_)) {
    error_ = "no sample size table";
    return false;
  }
  stszTable_ = stsz.payload + 12;

  if (findChild(stbl.payload, stbl.end, "stco", stco)) {
    chunkOffsets64_ = false;
  } else if (findChild(stbl.payload, stbl.end, "co64", stco)) {
    chunkOffsets64_ = true;
  } else {
    error_ = "no chunk offset table";
    return false;
  }
  if (!readU32(stco.payload + 4, chunkCount_)) return false;
  chunkTable_ = stco.payload + 8;
  return !stsc_.empty() && sampleCount_ > 0 && chunkCount_ > 0;
}

void Mp4Source::parseTitle(const Box& moov) {
  Box udta, meta, ilst, data;
  if (!findChild(moov.payload, moov.end, "udta", udta) || !findChild(udta.payload, udta.end, "meta", meta)) return;
  if (!findChild(meta.payload + 4, meta.end, "ilst", ilst)) return;  // meta is a FullBox
  const char name[4] = {static_cast<char>(0xA9), 'n', 'a', 'm'};
  Box box;
  uint64_t at = ilst.payload;
  while (readBox(at, ilst.end, box)) {
    if (std::memcmp(box.type, name, 4) == 0 && findChild(box.payload, box.end, "data", data)) {
      const size_t length = static_cast<size_t>(std::min<uint64_t>(data.end - data.payload - 8, 200));
      std::string text(length, '\0');
      if (file_.seekSet(data.payload + 8) && file_.read(&text[0], length) == static_cast<int>(length)) title_ = text;
      return;
    }
    at = box.end;
  }
}

bool Mp4Source::open(const std::string& path) {
  file_ = SdMan.open(path.c_str(), O_RDONLY);
  if (!file_) {
    error_ = "cannot open file";
    return false;
  }
  fileSize_ = file_.fileSize();
  Box moov;
  if (!findChild(0, fileSize_, "moov", moov)) {
    error_ = "not an MP4 file (no moov)";
    return false;
  }
  Box trak;
  uint64_t at = moov.payload;
  bool found = false;
  while (findChild(at, moov.end, "trak", trak)) {
    if (parseTrack(trak)) {
      found = true;
      break;
    }
    if (!error_.empty()) break;  // an audio track we cannot play
    at = trak.end;
  }
  if (!found) {
    if (error_.empty()) error_ = "no audio track";
    return false;
  }
  parseTitle(moov);
  pending_.resize(MAX_FRAME_BYTES + ADTS_HEADER);
  positionAt(0);
  return true;
}

uint32_t Mp4Source::sampleSize(const uint32_t sample) {
  if (fixedSampleSize_) return fixedSampleSize_;
  if (sample < sizeCacheFirst_ || sample >= sizeCacheFirst_ + CACHE_ENTRIES) {
    sizeCacheFirst_ = sample;
    const uint32_t entries = std::min(CACHE_ENTRIES, sampleCount_ - sample);
    uint8_t raw[CACHE_ENTRIES * 4];
    file_.seekSet(stszTable_ + static_cast<uint64_t>(sample) * 4);
    const int got = file_.read(raw, entries * 4);
    for (uint32_t i = 0; i < CACHE_ENTRIES; ++i) {
      sizeCache_[i] = static_cast<int>(i * 4 + 4) <= got ? be32(raw + i * 4) : 0;
    }
  }
  return sizeCache_[sample - sizeCacheFirst_];
}

uint64_t Mp4Source::chunkOffset(const uint32_t chunk) {
  if (chunk >= chunkCount_) return fileSize_;
  if (chunk < offsetCacheFirst_ || chunk >= offsetCacheFirst_ + CACHE_ENTRIES) {
    offsetCacheFirst_ = chunk;
    const uint32_t entries = std::min(CACHE_ENTRIES, chunkCount_ - chunk);
    const uint32_t width = chunkOffsets64_ ? 8 : 4;
    uint8_t raw[CACHE_ENTRIES * 8];
    file_.seekSet(chunkTable_ + static_cast<uint64_t>(chunk) * width);
    const int got = file_.read(raw, entries * width);
    for (uint32_t i = 0; i < CACHE_ENTRIES; ++i) {
      if (static_cast<int>((i + 1) * width) > got) {
        offsetCache_[i] = fileSize_;
      } else {
        offsetCache_[i] = chunkOffsets64_ ? be64(raw + i * 8) : be32(raw + i * 4);
      }
    }
  }
  return offsetCache_[chunk - offsetCacheFirst_];
}

void Mp4Source::positionAt(const uint32_t sample) {
  sample_ = std::min(sample, sampleCount_);
  pendingSize_ = pendingRead_ = 0;
  // Walk the sample-to-chunk runs to the chunk holding this sample.
  uint32_t runFirstSample = 0;
  for (stscIndex_ = 0; stscIndex_ < stsc_.size(); ++stscIndex_) {
    const uint32_t first = stsc_[stscIndex_].firstChunk - 1;
    const uint32_t next = stscIndex_ + 1 < stsc_.size() ? stsc_[stscIndex_ + 1].firstChunk - 1 : chunkCount_;
    const uint32_t perChunk = std::max<uint32_t>(1, stsc_[stscIndex_].samplesPerChunk);
    const uint64_t runSamples = static_cast<uint64_t>(next - first) * perChunk;
    if (sample_ < runFirstSample + runSamples || stscIndex_ + 1 == stsc_.size()) {
      chunk_ = first + (sample_ - runFirstSample) / perChunk;
      sampleInChunk_ = (sample_ - runFirstSample) % perChunk;
      break;
    }
    runFirstSample += static_cast<uint32_t>(runSamples);
  }
  offset_ = chunkOffset(chunk_);
  for (uint32_t i = sample_ - sampleInChunk_; i < sample_; ++i) offset_ += sampleSize(i);
}

uint64_t Mp4Source::sampleTimeUnits(const uint32_t sample) const {
  uint64_t units = 0;
  uint32_t remaining = sample;
  for (const SttsEntry& e : stts_) {
    const uint32_t take = std::min(remaining, e.count);
    units += static_cast<uint64_t>(take) * e.delta;
    remaining -= take;
    if (remaining == 0) break;
  }
  return units;
}

uint32_t Mp4Source::sampleAtTime(uint64_t units) const {
  uint32_t sample = 0;
  for (const SttsEntry& e : stts_) {
    const uint64_t span = static_cast<uint64_t>(e.count) * e.delta;
    if (units < span || e.delta == 0) return sample + static_cast<uint32_t>(e.delta ? units / e.delta : 0);
    units -= span;
    sample += e.count;
  }
  return sample;
}

uint32_t Mp4Source::seek(const uint32_t ms) {
  const uint32_t sample = sampleAtTime(static_cast<uint64_t>(ms) * timescale_ / 1000);
  positionAt(sample);
  return static_cast<uint32_t>(sampleTimeUnits(sample_) * 1000 / timescale_);
}

size_t Mp4Source::read(uint8_t* out, const size_t max) {
  size_t written = 0;
  while (written < max) {
    if (pendingRead_ == pendingSize_) {
      if (sample_ >= sampleCount_) break;
      const uint32_t size = sampleSize(sample_);
      const uint32_t perChunk = std::max<uint32_t>(1, stsc_[stscIndex_].samplesPerChunk);
      if (size > 0 && size <= MAX_FRAME_BYTES && file_.seekSet(offset_) &&
          file_.read(pending_.data() + ADTS_HEADER, size) == static_cast<int>(size)) {
        const uint32_t frameLength = size + ADTS_HEADER;
        uint8_t* h = pending_.data();
        h[0] = 0xFF;
        h[1] = 0xF1;  // MPEG-4, no CRC
        h[2] = static_cast<uint8_t>((adtsProfile_ << 6) | (sampleRateIndex_ << 2) | ((channelConfig_ >> 2) & 1));
        h[3] = static_cast<uint8_t>(((channelConfig_ & 3) << 6) | (frameLength >> 11));
        h[4] = static_cast<uint8_t>(frameLength >> 3);
        h[5] = static_cast<uint8_t>(((frameLength & 7) << 5) | 0x1F);
        h[6] = 0xFC;
        pendingSize_ = frameLength;
        pendingRead_ = 0;
      }
      // Advance the cursor whether or not the frame was usable, so a bad sample is skipped.
      offset_ += size;
      ++sample_;
      if (++sampleInChunk_ >= perChunk) {
        sampleInChunk_ = 0;
        ++chunk_;
        if (stscIndex_ + 1 < stsc_.size() && chunk_ + 1 >= stsc_[stscIndex_ + 1].firstChunk) ++stscIndex_;
        offset_ = chunkOffset(chunk_);
      }
      if (pendingRead_ == pendingSize_) continue;
    }
    const size_t n = std::min(max - written, pendingSize_ - pendingRead_);
    std::memcpy(out + written, pending_.data() + pendingRead_, n);
    pendingRead_ += n;
    written += n;
  }
  return written;
}

#endif  // INX_HAS_AUDIO_MODULE
