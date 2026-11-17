/**
 * @file audio_decode_host_test.cpp
 * @brief Host check of PcmDecoder (src/audio/PcmDecoder.h): decodes an MP3 or ADTS file fed in
 *        random-sized pieces, as the device's ring buffer hands it over, and writes raw PCM.
 *
 * Usage: audio_decode_host_test mp3|aac <input> <output.s16> [seed]
 *        audio_decode_host_test compare <reference.s16> <output.s16> <channels>
 *        audio_decode_host_test resample <input.s16> <inRate> <outRate> <output.s16>
 * The first prints the output rate, frames, errors and rate changes. The second prints the SNR of
 * the output against ffmpeg's decode of the same file (mono: ffmpeg's mono decode against our
 * left channel, since ffmpeg mixes mono to stereo at -3 dB), at the best alignment.
 */

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "../../src/audio/PcmDecoder.h"
#include "../../src/audio/Resampler.h"

namespace {

std::vector<int16_t> readPcm(const char* path) {
  std::vector<int16_t> out;
  FILE* f = fopen(path, "rb");
  if (!f) return out;
  int16_t buf[4096];
  size_t n;
  while ((n = fread(buf, sizeof(int16_t), 4096, f)) > 0) out.insert(out.end(), buf, buf + n);
  fclose(f);
  return out;
}

/** SNR in dB of out against ref at the best offset (decoders differ in the priming they emit). */
int compare(const char* refPath, const char* outPath, const int channels) {
  const std::vector<int16_t> ref = readPcm(refPath);
  const std::vector<int16_t> stereo = readPcm(outPath);
  std::vector<int16_t> out;
  if (channels == 1) {
    for (size_t i = 0; i < stereo.size(); i += 2) out.push_back(stereo[i]);
  } else {
    out = stereo;
  }
  const long step = channels;  // one frame
  double best = -1e9;
  long bestOffset = 0;
  for (long off = -4096; off <= 4096; ++off) {
    const long a0 = std::max(0L, off) * step, b0 = std::max(0L, -off) * step;
    const long n = std::min<long>(static_cast<long>(ref.size()) - a0, static_cast<long>(out.size()) - b0);
    if (n < 40000) continue;
    double sig = 0, err = 0;
    for (long i = 20000; i < n; i += 5) {
      const double x = ref[a0 + i], y = out[b0 + i];
      sig += x * x;
      err += (x - y) * (x - y);
    }
    const double snr = err > 0 ? 10 * log10(sig / err) : 200;
    if (snr > best) {
      best = snr;
      bestOffset = off;
    }
  }
  printf("snr=%.1fdB offset=%ld frames ref=%zu out=%zu\n", best, bestOffset, ref.size() / channels, stereo.size() / 2);
  return 0;
}

/** Stereo s16 through Resampler in random-sized pieces, as the device's decoded frames arrive. */
int resample(const char* inPath, const uint32_t inRate, const uint32_t outRate, const char* outPath) {
  const std::vector<int16_t> in = readPcm(inPath);
  Resampler resampler;
  if (!resampler.configure(inRate, outRate, malloc, free)) return 1;
  std::vector<int16_t> out(resampler.maxOutput(Resampler::MAX_INPUT_FRAMES) * 2);
  FILE* f = fopen(outPath, "wb");
  size_t done = 0, produced = 0;
  const size_t frames = in.size() / 2;
  while (done < frames) {
    const size_t n = std::min<size_t>(1 + rand() % Resampler::MAX_INPUT_FRAMES, frames - done);
    const size_t got = resampler.process(in.data() + 2 * done, n, out.data());
    fwrite(out.data(), sizeof(int16_t), got * 2, f);
    done += n;
    produced += got;
  }
  fclose(f);
  printf("in=%zu out=%zu expected=%llu\n", frames, produced,
         static_cast<unsigned long long>(static_cast<uint64_t>(frames) * outRate / inRate));
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc == 5 && strcmp(argv[1], "compare") == 0) return compare(argv[2], argv[3], atoi(argv[4]));
  if (argc == 6 && strcmp(argv[1], "resample") == 0) {
    return resample(argv[2], static_cast<uint32_t>(atoi(argv[3])), static_cast<uint32_t>(atoi(argv[4])), argv[5]);
  }
  if (argc < 4) {
    fprintf(stderr, "usage: %s mp3|aac <input> <output.s16> [seed]\n", argv[0]);
    return 2;
  }
  const PcmDecoder::Codec codec = strcmp(argv[1], "mp3") == 0 ? PcmDecoder::Codec::Mp3 : PcmDecoder::Codec::AacAdts;
  srand(argc > 4 ? static_cast<unsigned>(atoi(argv[4])) : 1);

  FILE* in = fopen(argv[2], "rb");
  FILE* out = fopen(argv[3], "wb");
  if (!in || !out) {
    perror("open");
    return 2;
  }
  std::vector<uint8_t> file;
  uint8_t chunk[4096];
  size_t n;
  while ((n = fread(chunk, 1, sizeof(chunk), in)) > 0) file.insert(file.end(), chunk, chunk + n);
  fclose(in);

  PcmDecoder decoder;
  if (!decoder.begin(codec, malloc, free)) {
    fprintf(stderr, "begin failed\n");
    return 1;
  }

  // The device keeps a window of up to 16 KB; arrivals are 1..2048 bytes.
  constexpr size_t WINDOW = 16 * 1024;
  std::vector<uint8_t> window;
  std::vector<int16_t> pcm(PcmDecoder::MAX_FRAMES * 2);
  size_t fed = 0;
  unsigned long frames = 0, errors = 0, decodes = 0;
  uint32_t rate = 0;
  int rateChanges = 0;
  uint8_t channels = 0;
  for (;;) {
    while (fed < file.size() && window.size() < WINDOW) {
      const size_t piece = std::min<size_t>(1 + rand() % 2048, std::min(file.size() - fed, WINDOW - window.size()));
      window.insert(window.end(), file.begin() + fed, file.begin() + fed + piece);
      fed += piece;
      if (rand() % 3) break;  // decode with a partly filled window too
    }
    const bool final = fed == file.size();
    if (window.empty()) break;
    const PcmDecoder::Frame f = decoder.decode(window.data(), window.size(), pcm.data(), final);
    if (f.consumed == 0) {
      if (final) break;
      continue;
    }
    window.erase(window.begin(), window.begin() + f.consumed);
    if (f.error) ++errors;
    if (f.frames) {
      ++decodes;
      if (rate && f.sampleRate != rate) {
        ++rateChanges;
        fprintf(stderr, "rate change %u -> %u after %lu frames\n", rate, f.sampleRate, frames);
      }
      rate = f.sampleRate;
      channels = f.channels;
      fwrite(pcm.data(), sizeof(int16_t), f.frames * 2, out);
      frames += f.frames;
    }
  }
  fclose(out);
  printf("rate=%u channels=%u frames=%lu decodes=%lu errors=%lu rate_changes=%d\n", rate, channels, frames, decodes,
         errors, rateChanges);
  return 0;
}
