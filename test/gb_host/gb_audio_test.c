/**
 * @file gb_audio_test.c
 * @brief Host check of the Game Boy sound path: runs a ROM with music through GbSession with sound
 *        on, as the device does, and checks that the result is music rather than noise.
 *
 * Usage: gb_audio_test <rom> <frames> [out.wav|-] [expected_hz]
 *
 * Presses Start now and then to get past title screens. Over the second half it measures how
 * often the signal crosses zero, as a rough pitch: game music sits in the low hundreds of Hz.
 * The core's half-rate mode (preferences_sample_rate 1) came out at 4-5x that, which is noise
 * and was heard as crackle; this fails above MAX_CROSSING_HZ. Also fails on silence.
 *
 * With expected_hz (a held note, test/gb_host/make_tone_rom.py), the rate must be within 2% of
 * it instead: a square wave crosses zero exactly twice a cycle. The core's mono render once
 * glitched at its 256-sample chunk joins, which doubled the rate of a 65 Hz note.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "../../src/games/gb/GbSession.h"

#define MAX_CROSSING_HZ 600.0

int main(int argc, char** argv) {
  if (argc < 3) return fprintf(stderr, "usage: %s <rom> <frames> [out.wav|-] [expected_hz]\n", argv[0]), 2;
  const double expected = argc > 4 ? atof(argv[4]) : 0.0;
  FILE* f = fopen(argv[1], "rb");
  if (!f) return perror(argv[1]), 1;
  fseek(f, 0, SEEK_END);
  const long size = ftell(f);
  fseek(f, 0, SEEK_SET);
  uint8_t* rom = malloc((size_t)size);
  if (!rom || fread(rom, 1, (size_t)size, f) != (size_t)size) return fprintf(stderr, "read failed\n"), 1;
  fclose(f);
  if (!gbs_open(rom, (size_t)size, true)) return 1;

  const int frames = atoi(argv[2]);
  int16_t* audio = malloc(sizeof(int16_t) * 740 * (size_t)frames);
  size_t n = 0;
  size_t half = 0;
  uint32_t due = 0;
  const uint32_t per_frame = (uint32_t)((uint64_t)GBS_AUDIO_RATE * 70224 * 65536 / 4194304);  // as GameBoy.cpp
  for (int i = 0; i < frames; ++i) {
    gbs_set_buttons(i % 300 < 4 ? GBS_START : 0);
    gbs_run_frame();
    due += per_frame;
    const int samples = (int)(due >> 16);
    due &= 0xFFFF;
    if (i == frames / 2) half = n;
    gbs_render_audio(audio + n, samples);
    n += (size_t)samples;
  }

  if (argc > 3 && argv[3][0] != '-') {
    FILE* out = fopen(argv[3], "wb");
    if (!out) return perror(argv[3]), 1;
    const uint32_t bytes = (uint32_t)(n * 2), rate = GBS_AUDIO_RATE, byte_rate = GBS_AUDIO_RATE * 2;
    const uint32_t riff = 36 + bytes, fmt_size = 16;
    const uint16_t pcm = 1, channels = 1, align = 2, bits = 16;
    fwrite("RIFF", 1, 4, out);
    fwrite(&riff, 4, 1, out);
    fwrite("WAVEfmt ", 1, 8, out);
    fwrite(&fmt_size, 4, 1, out);
    fwrite(&pcm, 2, 1, out);
    fwrite(&channels, 2, 1, out);
    fwrite(&rate, 4, 1, out);
    fwrite(&byte_rate, 4, 1, out);
    fwrite(&align, 2, 1, out);
    fwrite(&bits, 2, 1, out);
    fwrite("data", 1, 4, out);
    fwrite(&bytes, 4, 1, out);
    fwrite(audio, 2, n, out);
    fclose(out);
  }

  long crossings = 0;
  long loud = 0;
  for (size_t i = half + 1; i < n; ++i) {
    if ((audio[i - 1] < 0) != (audio[i] < 0)) ++crossings;
    if (abs(audio[i]) > 1000) ++loud;
  }
  const double seconds = (double)(n - half) / GBS_AUDIO_RATE;
  const double hz = crossings / 2.0 / seconds;
  int ok;
  if (expected > 0) {
    ok = loud > 0 && fabs(hz - expected) <= expected * 0.02;
    printf("GB sound (%s): %.1f Hz (expected %.1f), %s: %s\n", argv[1], hz, expected, loud ? "not silent" : "SILENT",
           ok ? "ok" : "FAIL");
  } else {
    ok = loud > 0 && hz <= MAX_CROSSING_HZ;
    printf("GB sound (%s): %.0f Hz zero-crossing rate (at most %.0f), %s: %s\n", argv[1], hz, MAX_CROSSING_HZ,
           loud ? "not silent" : "SILENT", ok ? "ok" : "FAIL");
  }
  gbs_close();
  free(audio);
  free(rom);
  return ok ? 0 : 1;
}
