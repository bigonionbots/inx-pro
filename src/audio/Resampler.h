#pragma once

/**
 * @file Resampler.h
 * @brief Stereo 16-bit sample rate conversion for USB headphones (UsbAudioSink.h).
 *
 * USB audio adapters take only a few rates (the AU-UCMA: 44.1, 48 and 96 kHz), while books come
 * at anything from 8 to 48 kHz, low-rate speech often at 22.05 or 16 kHz. This is a polyphase
 * windowed-sinc filter (16 taps, 128 phases, interpolated) with exact rational stepping, so the
 * output never drifts from the input timeline. Equal rates pass straight through.
 *
 * No Arduino or ESP-IDF dependencies (test/audio_decode_host checks it on a PC).
 */

#include <cstddef>
#include <cstdint>

class Resampler {
 public:
  /** Most input frames one process() call takes. */
  static constexpr size_t MAX_INPUT_FRAMES = 2048;

  Resampler() = default;
  ~Resampler();
  Resampler(const Resampler&) = delete;
  Resampler& operator=(const Resampler&) = delete;

  /**
   * Sets the rates and clears the history. Memory comes from `allocate` (once). False if a rate
   * is 0 or an allocation failed.
   */
  bool configure(uint32_t inRate, uint32_t outRate, void* (*allocate)(size_t), void (*release)(void*));
  /** Clears the history (after a seek), keeping the rates. */
  void reset();

  /** Output frames that process() can produce from inFrames, at most (for sizing `out`). */
  size_t maxOutput(size_t inFrames) const;

  /**
   * Converts interleaved stereo frames (inFrames <= MAX_INPUT_FRAMES). The output may lag the input by
   * half the filter (8 input frames); the rest comes out with the next call.
   */
  size_t process(const int16_t* in, size_t inFrames, int16_t* out);

  uint32_t inRate() const { return inRate_; }
  uint32_t outRate() const { return outRate_; }
  bool passthrough() const { return inRate_ == outRate_; }

 private:
  static constexpr int TAPS = 16;
  static constexpr int PHASES = 128;
  static constexpr int HISTORY = TAPS / 2 - 1;  ///< Input frames before the current position

  void buildFilter();

  uint32_t inRate_ = 0;
  uint32_t outRate_ = 0;
  void (*release_)(void*) = nullptr;
  float* filter_ = nullptr;  ///< (PHASES + 1) x TAPS
  float* buffer_ = nullptr;  ///< Stereo input frames, HISTORY + MAX_INPUT_FRAMES + TAPS
  size_t frames_ = 0;        ///< Frames in buffer_
  size_t position_ = 0;      ///< Integer input position of the next output frame
  uint64_t fraction_ = 0;    ///< Its fractional part, in units of 1 / outRate_
};
