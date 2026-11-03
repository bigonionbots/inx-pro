/**
 * @file overlay_png_dump.cpp
 * @brief Host check of OverlayPng::decode(): decodes a 960x540 PNG, writes its two planes as PBM.
 *
 * Usage: overlay_png_dump <in.png> <ink.pbm> <opaque.pbm>. PBM 1 = black, so the ink plane is
 * written inverted (FastEPD's 1 = white) and the opaque plane as is (1 = drawn shows black).
 */

#include <cstdio>
#include <vector>

#include "../../src/games/OverlayPng.h"

static bool writePbm(const char* path, const std::vector<uint8_t>& plane, const bool invert) {
  FILE* f = std::fopen(path, "wb");
  if (!f) return false;
  std::fprintf(f, "P4\n960 540\n");
  for (const uint8_t b : plane) std::fputc(invert ? static_cast<uint8_t>(~b) : b, f);
  std::fclose(f);
  return true;
}

int main(int argc, char** argv) {
  if (argc != 4) {
    std::fprintf(stderr, "usage: %s <in.png> <ink.pbm> <opaque.pbm>\n", argv[0]);
    return 2;
  }
  FILE* f = std::fopen(argv[1], "rb");
  if (!f) return 1;
  std::vector<uint8_t> png;
  for (int c = std::fgetc(f); c != EOF; c = std::fgetc(f)) png.push_back(static_cast<uint8_t>(c));
  std::fclose(f);
  std::vector<uint8_t> ink(960 * 540 / 8), opaque(960 * 540 / 8);
  const OverlayPng::Error err = OverlayPng::decode(png.data(), png.size(), 960, 540, ink.data(), opaque.data());
  if (err != OverlayPng::Error::None) {
    std::printf("%s: %s\n", argv[1], OverlayPng::describe(err));
    return 3;
  }
  return writePbm(argv[2], ink, true) && writePbm(argv[3], opaque, false) ? 0 : 1;
}
