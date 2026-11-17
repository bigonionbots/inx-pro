#pragma once

/**
 * @file PcmDecoder.h
 * @brief MP3 and AAC (ADTS) to 16-bit PCM on the T5, for USB headphones (UsbAudioSink.h).
 *
 * The same compressed streams the audio module decodes (AudioLinkProtocol.h: Codec::Mp3 and
 * Codec::AacAdts, as Mp3Source and Mp4Source produce them). MP3 goes through minimp3 (CC0,
 * third_party/minimp3); AAC-LC, HE-AAC and HE-AAC v2 through the PacketVideo decoder from
 * Android (Apache-2.0, third_party/opencore_aacdec). Output is always interleaved stereo: mono
 * sources are duplicated.
 *
 * No Arduino or ESP-IDF dependencies, so test/audio_decode_host builds it on a PC. Not
 * thread-safe: one task owns a decoder.
 */

#include <cstddef>
#include <cstdint>

class PcmDecoder {
 public:
  enum class Codec : uint8_t { None, Mp3, AacAdts };

  /** Most stereo frames one decode() can produce: HE-AAC's 2048 (MP3: 1152). */
  static constexpr size_t MAX_FRAMES = 2048;

  struct Frame {
    size_t consumed = 0;      ///< Input bytes used (frame and any junk skipped before it)
    size_t frames = 0;        ///< Stereo frames written to pcm, 0 if none
    uint32_t sampleRate = 0;  ///< Of this output
    uint8_t channels = 0;     ///< In the source (the output is stereo regardless)
    bool error = false;       ///< The frame was damaged and skipped (consumed covers it)
  };

  PcmDecoder();
  ~PcmDecoder();
  PcmDecoder(const PcmDecoder&) = delete;
  PcmDecoder& operator=(const PcmDecoder&) = delete;

  /**
   * Prepares for a new stream. Allocations come from `allocate` (PSRAM on the device); false if
   * one failed. Calling it again for another stream reuses the memory.
   */
  bool begin(Codec codec, void* (*allocate)(size_t bytes), void (*release)(void* block));
  /** Frees the decoder memory. */
  void end();

  /**
   * Decodes the first frame in data. `pcm` needs room for MAX_FRAMES stereo frames. With
   * consumed == 0, data holds no complete frame yet: call again with more. `final` says no more
   * input will follow, so a last frame shorter than its header claims is not waited for.
   */
  Frame decode(const uint8_t* data, size_t length, int16_t* pcm, bool final = false);

  /** Forget the bit reservoir and filter state (after a seek). */
  void reset();

  Codec codec() const { return codec_; }

 private:
  Frame decodeMp3(const uint8_t* data, size_t length, int16_t* pcm);
  Frame decodeAac(const uint8_t* data, size_t length, int16_t* pcm, bool final);
  bool configureAac(const uint8_t* adtsHeader);

  Codec codec_ = Codec::None;
  void* (*allocate_)(size_t) = nullptr;
  void (*release_)(void*) = nullptr;

  void* mp3_ = nullptr;  ///< mp3dec_t
  bool mp3Synced_ = false;

  void* aacConfig_ = nullptr;  ///< tPVMP4AudioDecoderExternal
  void* aacMemory_ = nullptr;
  uint32_t aacKey_ = 0;  ///< profile, rate index and channel config the decoder was set up for
  bool aacConfigured_ = false;
  int aacFramesSinceConfig_ = 0;
};
