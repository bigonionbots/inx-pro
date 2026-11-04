/**
 * @file live_audio_test.cpp
 * @brief Host check of game sound's path over the module link: MuLaw.h and Resample2x.h.
 *
 * Usage: live_audio_test [game.wav]
 *
 * µ-law: the signal-to-noise ratio on a sine and on Game Boy-like square waves (hard edges, where
 * IMA ADPCM, the other candidate, managed only 14 dB), that every byte decodes back to itself,
 * and that the extremes clip instead of wrapping.
 *
 * The whole path (44.1 kHz -> Decimator -> µ-law -> Interpolator -> 44.1 kHz), against the input
 * band-limited to 10 kHz (the most a 22050 Hz stream can carry), on synthetic squares and, given
 * a 44.1 kHz mono WAV of game sound (gb_audio_test writes one), on that. Pair averaging and
 * linear interpolation, which this replaced, scored 12.9 dB on uCity's music and crackled.
 */

#include <MuLaw.h>
#include <Resample2x.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

constexpr double RATE = 22050.0;
constexpr size_t SAMPLES = 22050;

bool roundTrip(const char* name, const std::vector<int16_t>& in, const double minSnr) {
  double signal = 0.0;
  double noise = 0.0;
  for (const int16_t s : in) {
    const double e = static_cast<double>(s) - mulaw::decode(mulaw::encode(s));
    signal += static_cast<double>(s) * s;
    noise += e * e;
  }
  const double snr = 10.0 * std::log10(signal / (noise > 0 ? noise : 1e-9));
  const bool ok = snr >= minSnr;
  std::printf("mu-law %s: SNR %.1f dB (at least %.0f): %s\n", name, snr, minSnr, ok ? "ok" : "FAIL");
  return ok;
}

/** Windowed-sinc low-pass (255 taps): the ideal band limit the path is measured against. */
std::vector<double> bandLimit(const std::vector<int16_t>& in) {
  constexpr int N = 255;
  constexpr double CUT = 10000.0 / 44100.0;
  std::vector<double> h(N);
  double sum = 0;
  for (int i = 0; i < N; ++i) {
    const int k = i - N / 2;
    const double x = k == 0 ? 2 * CUT : std::sin(2 * M_PI * CUT * k) / (M_PI * k);
    h[i] = x * (0.42 - 0.5 * std::cos(2 * M_PI * i / (N - 1)) + 0.08 * std::cos(4 * M_PI * i / (N - 1)));
    sum += h[i];
  }
  std::vector<double> out(in.size(), 0.0);
  for (size_t n = 0; n < in.size(); ++n) {
    double acc = 0;
    for (int k = 0; k < N; ++k) {
      const long i = static_cast<long>(n) + N / 2 - k;
      if (i >= 0 && i < static_cast<long>(in.size())) acc += h[k] / sum * in[i];
    }
    out[n] = acc;
  }
  return out;
}

/** The live path, as the T5 (Decimator, µ-law) and the module (Interpolator) run it. */
std::vector<int16_t> livePath(const std::vector<int16_t>& in) {
  resample2x::Decimator down;
  resample2x::Interpolator up;
  std::vector<int16_t> out;
  out.reserve(in.size());
  for (const int16_t s : in) {
    int16_t half = 0;
    if (!down.push(s, half)) continue;
    int16_t a = 0;
    int16_t b = 0;
    up.push(mulaw::decode(mulaw::encode(half)), a, b);
    out.push_back(a);
    out.push_back(b);
  }
  return out;
}

/** SNR of the live path against the band-limited input, at the best of a few delays. */
bool pathCheck(const char* name, const std::vector<int16_t>& in, const double minSnr) {
  const std::vector<double> ref = bandLimit(in);
  const std::vector<int16_t> out = livePath(in);
  double best = -1e9;
  int bestDelay = 0;
  for (int delay = 0; delay <= 80; ++delay) {
    double signal = 0;
    double noise = 0;
    for (size_t i = 2000; i + 2000 < out.size() && i + delay < out.size(); ++i) {
      const double e = out[i + delay] - ref[i];
      signal += ref[i] * ref[i];
      noise += e * e;
    }
    const double snr = 10.0 * std::log10(signal / (noise > 0 ? noise : 1e-9));
    if (snr > best) {
      best = snr;
      bestDelay = delay;
    }
  }
  const bool ok = best >= minSnr;
  std::printf("live path %s: SNR %.1f dB at %d samples delay (at least %.0f): %s\n", name, best, bestDelay, minSnr,
              ok ? "ok" : "FAIL");
  return ok;
}

}  // namespace

int main(int argc, char** argv) {
  bool ok = true;
  std::vector<int16_t> tone(SAMPLES);
  for (size_t i = 0; i < SAMPLES; ++i) tone[i] = static_cast<int16_t>(12000 * std::sin(2 * M_PI * 440.0 * i / RATE));
  ok &= roundTrip("440 Hz sine", tone, 35.0);

  // Game Boy channels are square waves; two at once, at the APU's typical levels.
  std::vector<int16_t> square(SAMPLES);
  for (size_t i = 0; i < SAMPLES; ++i) {
    const int a = std::fmod(i * 523.25 / RATE, 1.0) < 0.5 ? 4000 : -4000;
    const int b = std::fmod(i * 784.0 / RATE, 1.0) < 0.25 ? 3000 : -3000;
    square[i] = static_cast<int16_t>(a + b);
  }
  ok &= roundTrip("GB-like squares", square, 35.0);

  bool stable = true;
  for (int b = 0; b < 256; ++b) {
    // 0x7F and 0xFF are +0 and -0; both decode to 0, which encodes as 0xFF.
    const uint8_t again = mulaw::encode(mulaw::decode(static_cast<uint8_t>(b)));
    stable &= again == b || (b == 0x7F && again == 0xFF);
  }
  std::printf("mu-law codes decode and encode back: %s\n", stable ? "ok" : "FAIL");
  ok &= stable;

  const bool clips = mulaw::decode(mulaw::encode(32767)) > 30000 && mulaw::decode(mulaw::encode(-32768)) < -30000;
  std::printf("mu-law extremes clip: %s\n", clips ? "ok" : "FAIL");
  ok &= clips;

  // The live path on squares at 44.1 kHz (as the core makes them), then on real game sound.
  std::vector<int16_t> square44(44100);
  for (size_t i = 0; i < square44.size(); ++i) {
    const int a = std::fmod(i * 523.25 / 44100.0, 1.0) < 0.5 ? 4000 : -4000;
    const int b = std::fmod(i * 1567.98 / 44100.0, 1.0) < 0.25 ? 3000 : -3000;
    square44[i] = static_cast<int16_t>(a + b);
  }
  ok &= pathCheck("GB-like squares", square44, 25.0);
  if (argc > 1) {
    FILE* f = std::fopen(argv[1], "rb");
    if (!f) return std::perror(argv[1]), 1;
    std::fseek(f, 0, SEEK_END);
    const long samples = (std::ftell(f) - 44) / 2;
    std::fseek(f, 44, SEEK_SET);
    std::vector<int16_t> wav(static_cast<size_t>(samples));
    if (std::fread(wav.data(), 2, wav.size(), f) != wav.size()) return 1;
    std::fclose(f);
    // 20 s from the middle, where the music plays
    const size_t from = wav.size() / 2;
    const std::vector<int16_t> part(wav.begin() + from, wav.begin() + std::min(wav.size(), from + 20 * 44100));
    ok &= pathCheck("game music", part, 25.0);
  }
  return ok ? 0 : 1;
}
