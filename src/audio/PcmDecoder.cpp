/**
 * @file PcmDecoder.cpp
 * @brief Definitions for PcmDecoder.
 */

#include "PcmDecoder.h"

// The firmware builds it only with the audio module support (its decoders are linked there); the
// host test defines PCM_DECODER_HOST.
#if INX_HAS_AUDIO_MODULE || PCM_DECODER_HOST

#include <cstring>

// The PacketVideo headers' export macros, empty as in Android's build.
#ifndef OSCL_IMPORT_REF
#define OSCL_IMPORT_REF
#endif
#ifndef OSCL_EXPORT_REF
#define OSCL_EXPORT_REF
#endif

#define MINIMP3_IMPLEMENTATION
#define MINIMP3_ONLY_MP3
#include "../../third_party/minimp3/minimp3.h"
#include "../../third_party/opencore_aacdec/src/e_tmp4audioobjecttype.h"
#include "../../third_party/opencore_aacdec/src/pvmp4audiodecoder_api.h"

namespace {

constexpr size_t ADTS_HEADER = 7;
constexpr size_t ADTS_HEADER_CRC = 9;
/** Rate indices 0..12 (ISO 14496-3 table 1.18); 13-15 are reserved. */
constexpr uint8_t ADTS_RATE_INDICES = 13;
/** A frame header followed by this much junk without a frame means the stream is not ADTS. */
constexpr size_t MAX_SCAN = 64 * 1024;

bool isAdtsSync(const uint8_t* p) { return p[0] == 0xFF && (p[1] & 0xF6) == 0xF0; }

size_t adtsFrameLength(const uint8_t* p) {
  return (static_cast<size_t>(p[3] & 0x03) << 11) | (static_cast<size_t>(p[4]) << 3) | (p[5] >> 5);
}

/** Profile (object type - 1), rate index and channel configuration: what the decoder is set up for. */
uint32_t adtsKey(const uint8_t* p) {
  const uint32_t profile = p[2] >> 6;
  const uint32_t rate = (p[2] >> 2) & 0x0F;
  const uint32_t channels = ((p[2] & 0x01) << 2) | (p[3] >> 6);
  return (profile << 8) | (rate << 4) | channels;
}

/** A plausible header: valid rate index, a length that covers at least the header. */
bool adtsHeaderValid(const uint8_t* p) {
  if (!isAdtsSync(p)) return false;
  if (((p[2] >> 2) & 0x0F) >= ADTS_RATE_INDICES) return false;
  const size_t header = (p[1] & 0x01) ? ADTS_HEADER : ADTS_HEADER_CRC;
  return adtsFrameLength(p) > header;
}

}  // namespace

PcmDecoder::PcmDecoder() = default;

PcmDecoder::~PcmDecoder() { end(); }

bool PcmDecoder::begin(const Codec codec, void* (*allocate)(size_t), void (*release)(void*)) {
  if (allocate_ != allocate || release_ != release) end();
  allocate_ = allocate;
  release_ = release;
  codec_ = codec;
  if (codec == Codec::Mp3) {
    if (!mp3_) {
      mp3_ = allocate_(sizeof(mp3dec_t));
      if (!mp3_) return false;
    }
    mp3dec_init(static_cast<mp3dec_t*>(mp3_));
    mp3Synced_ = false;
    return true;
  }
  if (codec == Codec::AacAdts) {
    if (!aacConfig_) {
      aacConfig_ = allocate_(sizeof(tPVMP4AudioDecoderExternal));
      if (!aacConfig_) return false;
    }
    if (!aacMemory_) {
      aacMemory_ = allocate_(PVMP4AudioDecoderGetMemRequirements());
      if (!aacMemory_) return false;
    }
    aacConfigured_ = false;
    aacKey_ = 0;
    return true;
  }
  return false;
}

void PcmDecoder::end() {
  if (release_) {
    if (mp3_) release_(mp3_);
    if (aacConfig_) release_(aacConfig_);
    if (aacMemory_) release_(aacMemory_);
  }
  mp3_ = nullptr;
  aacConfig_ = nullptr;
  aacMemory_ = nullptr;
  aacConfigured_ = false;
  codec_ = Codec::None;
}

void PcmDecoder::reset() {
  if (codec_ == Codec::Mp3 && mp3_) {
    mp3dec_init(static_cast<mp3dec_t*>(mp3_));
    mp3Synced_ = false;
  } else if (codec_ == Codec::AacAdts && aacMemory_ && aacConfigured_) {
    PVMP4AudioDecoderResetBuffer(aacMemory_);
  }
}

PcmDecoder::Frame PcmDecoder::decode(const uint8_t* data, const size_t length, int16_t* pcm, const bool final) {
  if (codec_ == Codec::Mp3 && mp3_) return decodeMp3(data, length, pcm);
  if (codec_ == Codec::AacAdts && aacMemory_) return decodeAac(data, length, pcm, final);
  Frame out;
  out.consumed = length;
  out.error = true;
  return out;
}

PcmDecoder::Frame PcmDecoder::decodeMp3(const uint8_t* data, const size_t length, int16_t* pcm) {
  Frame out;
  if (length == 0) return out;
  mp3dec_frame_info_t info{};
  // minimp3 writes interleaved samples for the source's channel count.
  const int samples = mp3dec_decode_frame(static_cast<mp3dec_t*>(mp3_), data, static_cast<int>(length), pcm, &info);
  out.consumed = static_cast<size_t>(info.frame_bytes);
  if (info.frame_bytes == 0) return out;  // needs more data (or none of it is MP3: caller decides)
  if (samples == 0) {
    // Skipped junk or ID3, or a frame it could not decode (the first frame after a seek may lack
    // its bit reservoir: that is normal, not an error).
    out.error = mp3Synced_ && info.channels > 0;
    return out;
  }
  mp3Synced_ = true;
  out.frames = static_cast<size_t>(samples);
  out.sampleRate = static_cast<uint32_t>(info.hz);
  out.channels = static_cast<uint8_t>(info.channels);
  if (info.channels == 1) {
    for (size_t i = out.frames; i-- > 0;) {
      pcm[2 * i] = pcm[i];
      pcm[2 * i + 1] = pcm[i];
    }
  }
  return out;
}

bool PcmDecoder::configureAac(const uint8_t* adtsHeader) {
  auto* config = static_cast<tPVMP4AudioDecoderExternal*>(aacConfig_);
  memset(config, 0, sizeof(*config));
  config->outputFormat = OUTPUTFORMAT_16PCM_INTERLEAVED;
  config->aacPlusEnabled = 1;
  config->desiredChannels = 2;
  if (PVMP4AudioDecoderInitLibrary(config, aacMemory_) != MP4AUDEC_SUCCESS) return false;

  // AudioSpecificConfig from the ADTS header: object type (5 bits), rate index (4), channels (4).
  const uint8_t objectType = static_cast<uint8_t>((adtsHeader[2] >> 6) + 1);
  const uint8_t rateIndex = (adtsHeader[2] >> 2) & 0x0F;
  const uint8_t channels = static_cast<uint8_t>(((adtsHeader[2] & 0x01) << 2) | (adtsHeader[3] >> 6));
  uint8_t asc[2] = {static_cast<uint8_t>((objectType << 3) | (rateIndex >> 1)),
                    static_cast<uint8_t>(((rateIndex & 0x01) << 7) | (channels << 3))};
  config->pInputBuffer = asc;
  config->inputBufferCurrentLength = sizeof(asc);
  config->inputBufferMaxLength = 0;
  if (PVMP4AudioDecoderConfig(config, aacMemory_) != MP4AUDEC_SUCCESS) return false;
  aacKey_ = adtsKey(adtsHeader);
  aacConfigured_ = true;
  aacFramesSinceConfig_ = 0;
  return true;
}

PcmDecoder::Frame PcmDecoder::decodeAac(const uint8_t* data, const size_t length, int16_t* pcm, const bool final) {
  Frame out;
  // Find a frame header. Junk before it is consumed on its own, so the caller can drop it.
  size_t skip = 0;
  while (skip + ADTS_HEADER <= length && !adtsHeaderValid(data + skip)) ++skip;
  if (skip > 0) {
    out.consumed = skip;
    out.error = aacConfigured_;
    return out;
  }
  if (length < ADTS_HEADER) {
    if (final) out.consumed = length;
    return out;
  }
  const uint8_t* header = data;
  const size_t frameLength = adtsFrameLength(header);
  if (frameLength > length) {
    if (final || length > MAX_SCAN) {
      out.consumed = length;
      out.error = true;
    }
    return out;
  }
  out.consumed = frameLength;

  if (!aacConfigured_ || adtsKey(header) != aacKey_) {
    if (!configureAac(header)) {
      aacConfigured_ = false;
      out.error = true;
      return out;
    }
  }

  const size_t headerLength = (header[1] & 0x01) ? ADTS_HEADER : ADTS_HEADER_CRC;
  auto* config = static_cast<tPVMP4AudioDecoderExternal*>(aacConfig_);
  config->pInputBuffer = const_cast<uint8_t*>(header + headerLength);
  config->inputBufferCurrentLength = static_cast<Int>(frameLength - headerLength);
  config->inputBufferMaxLength = 0;
  config->inputBufferUsedLength = 0;
  config->remainderBits = 0;
  config->pOutputBuffer = pcm;
  config->pOutputBuffer_plus = pcm + 2048;  // the second half of an HE-AAC frame
  config->repositionFlag = false;

  const Int err = PVMP4AudioDecodeFrame(config, aacMemory_);
  if (err != MP4AUDEC_SUCCESS) {
    out.error = true;
    return out;
  }

  const int factor = (config->aacPlusEnabled && config->aacPlusUpsamplingFactor == 2) ? 2 : 1;
  out.frames = static_cast<size_t>(config->frameLength) * static_cast<size_t>(factor);
  out.sampleRate = static_cast<uint32_t>(config->samplingRate);
  out.channels = static_cast<uint8_t>(config->encodedChannels);

  // As Android's SoftAAC: a low-rate AAC-LC stream is first decoded as if it carried SBR
  // (implicit signalling); once the second frame shows it does not, SBR is switched off so the
  // output drops to the core rate from the next frame. Explicit HE-AAC keeps it.
  if (++aacFramesSinceConfig_ == 2 && factor == 2 &&
      (config->extendedAudioObjectType == MP4AUDIO_AAC_LC || config->extendedAudioObjectType == MP4AUDIO_LTP)) {
    config->aacPlusEnabled = 0;
    config->aacPlusUpsamplingFactor = 1;
    config->samplingRate /= 2;
  }
  return out;
}

#endif  // INX_HAS_AUDIO_MODULE || PCM_DECODER_HOST
