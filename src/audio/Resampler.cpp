/**
 * @file Resampler.cpp
 * @brief Definitions for Resampler.
 */

#include "Resampler.h"

#if INX_HAS_AUDIO_MODULE || PCM_DECODER_HOST

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {

constexpr double PI = 3.14159265358979323846;

/** Kaiser window (beta 6: about 60 dB of stopband) at x in [-1, 1]. */
double kaiser(const double x) {
  constexpr double BETA = 6.0;
  auto bessel0 = [](const double v) {
    double sum = 1.0, term = 1.0;
    for (int k = 1; k < 25; ++k) {
      term *= (v / (2.0 * k)) * (v / (2.0 * k));
      sum += term;
    }
    return sum;
  };
  const double r = 1.0 - x * x;
  return r <= 0 ? 0.0 : bessel0(BETA * std::sqrt(r)) / bessel0(BETA);
}

}  // namespace

Resampler::~Resampler() {
  if (release_) {
    if (filter_) release_(filter_);
    if (buffer_) release_(buffer_);
  }
}

bool Resampler::configure(const uint32_t inRate, const uint32_t outRate, void* (*allocate)(size_t),
                          void (*release)(void*)) {
  if (inRate == 0 || outRate == 0) return false;
  release_ = release;
  if (!filter_) filter_ = static_cast<float*>(allocate(sizeof(float) * (PHASES + 1) * TAPS));
  if (!buffer_) buffer_ = static_cast<float*>(allocate(sizeof(float) * 2 * (HISTORY + MAX_INPUT_FRAMES + TAPS)));
  if (!filter_ || !buffer_) return false;
  const bool changed = inRate != inRate_ || outRate != outRate_;
  inRate_ = inRate;
  outRate_ = outRate;
  if (changed) buildFilter();
  reset();
  return true;
}

void Resampler::buildFilter() {
  // Cut off a little below the lower Nyquist frequency, in cycles per input sample.
  const double cutoff = 0.5 * std::min(1.0, static_cast<double>(outRate_) / inRate_) * 0.92;
  for (int p = 0; p <= PHASES; ++p) {
    const double fraction = static_cast<double>(p) / PHASES;
    double sum = 0;
    for (int j = 0; j < TAPS; ++j) {
      // Tap j weighs input frame (position - HISTORY + j); the output sits at position + fraction.
      const double x = (j - HISTORY) - fraction;
      const double sinc = x == 0 ? 1.0 : std::sin(2 * PI * cutoff * x) / (2 * PI * cutoff * x);
      const double h = 2 * cutoff * sinc * kaiser(x / (TAPS / 2.0));
      filter_[p * TAPS + j] = static_cast<float>(h);
      sum += h;
    }
    // Unity gain at DC for every phase, so the interpolation does not ripple.
    for (int j = 0; j < TAPS; ++j) filter_[p * TAPS + j] = static_cast<float>(filter_[p * TAPS + j] / sum);
  }
}

void Resampler::reset() {
  if (!buffer_) return;
  std::memset(buffer_, 0, sizeof(float) * 2 * HISTORY);
  frames_ = HISTORY;
  position_ = HISTORY;
  fraction_ = 0;
}

size_t Resampler::maxOutput(const size_t inFrames) const {
  if (passthrough() || inRate_ == 0) return inFrames;
  return static_cast<size_t>((static_cast<uint64_t>(inFrames + TAPS) * outRate_) / inRate_) + 2;
}

size_t Resampler::process(const int16_t* in, size_t inFrames, int16_t* out) {
  if (passthrough()) {
    std::memcpy(out, in, inFrames * 2 * sizeof(int16_t));
    return inFrames;
  }
  inFrames = std::min(inFrames, MAX_INPUT_FRAMES);
  for (size_t i = 0; i < inFrames; ++i) {
    buffer_[2 * (frames_ + i)] = in[2 * i];
    buffer_[2 * (frames_ + i) + 1] = in[2 * i + 1];
  }
  frames_ += inFrames;

  size_t produced = 0;
  // Output while the filter's last tap is inside the buffer.
  while (position_ + (TAPS - HISTORY) <= frames_) {
    const uint64_t scaled = fraction_ * PHASES;
    const int phase = static_cast<int>(scaled / outRate_);
    const float blend = static_cast<float>(scaled % outRate_) / static_cast<float>(outRate_);
    const float* a = filter_ + phase * TAPS;
    const float* b = a + TAPS;
    const float* x = buffer_ + 2 * (position_ - HISTORY);
    float left = 0, right = 0;
    for (int j = 0; j < TAPS; ++j) {
      const float h = a[j] + (b[j] - a[j]) * blend;
      left += h * x[2 * j];
      right += h * x[2 * j + 1];
    }
    out[2 * produced] = static_cast<int16_t>(std::clamp(std::lround(left), -32768L, 32767L));
    out[2 * produced + 1] = static_cast<int16_t>(std::clamp(std::lround(right), -32768L, 32767L));
    ++produced;
    fraction_ += inRate_;
    while (fraction_ >= outRate_) {
      fraction_ -= outRate_;
      ++position_;
    }
  }

  // Keep the history the next call's first outputs need.
  const size_t keepFrom = position_ - HISTORY;
  if (keepFrom > 0) {
    std::memmove(buffer_, buffer_ + 2 * keepFrom, sizeof(float) * 2 * (frames_ - keepFrom));
    frames_ -= keepFrom;
    position_ -= keepFrom;
  }
  return produced;
}

#endif  // INX_HAS_AUDIO_MODULE || PCM_DECODER_HOST
