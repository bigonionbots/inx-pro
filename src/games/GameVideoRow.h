#pragma once

/**
 * @file GameVideoRow.h
 * @brief Per-pixel drive logic of the game video scan, one panel row at a time.
 *
 * The scan keeps, for every pixel, the colour it is being driven to (`shown`) and how many
 * more scans it still needs (a 3-bit counter split over three bit planes). When the target
 * image changes a pixel, the pixel restarts with `passes` scans towards its new colour; after
 * that it gets no more drive, so pixels that stay the same are never over-driven.
 *
 * Buffers are 1 bit per pixel, MSB = leftmost pixel, 1 = white (FastEPD's 1-bpp layout). Drive
 * codes are 2 bits per pixel, leftmost pixel in the top bits of each byte: 1 = towards black,
 * 2 = towards white, 0 = leave alone (FastEPD's partial-update codes).
 *
 * Plain C++ with no device headers, so the host tests can check it.
 */

#include <cstdint>
#include <cstring>

namespace GameVideoRow {

constexpr int MAX_PASSES = 7;

/** Drive codes for 8 pixels, indexed by a mask of the pixels to drive: [0] black, [1] white. */
struct CodeTables {
  uint16_t code[2][256];

  CodeTables() {
    for (int mask = 0; mask < 256; ++mask) {
      for (int colour = 0; colour < 2; ++colour) {
        uint8_t out[2] = {0, 0};
        for (int px = 0; px < 8; ++px) {
          if (!(mask & (0x80 >> px))) continue;
          out[px / 4] |= static_cast<uint8_t>((colour == 0 ? 1 : 2) << ((3 - px % 4) * 2));
        }
        uint16_t packed;
        std::memcpy(&packed, out, sizeof(packed));
        code[colour][mask] = packed;
      }
    }
  }
};

/**
 * Updates one row's drive state from its target and writes its drive codes.
 *
 * @param bytes     row length in bytes of the 1-bpp buffers (8 pixels each)
 * @param target    the image to show
 * @param shown     colour each pixel is being driven to (updated)
 * @param c0,c1,c2  remaining-scan counter planes, bit 0..2 (updated)
 * @param codes     2 * bytes bytes of drive codes (written)
 * @param passes    scans per colour change, 1..MAX_PASSES
 * @return whether any pixel in the row still needs drive after this scan
 */
inline bool prepare(const CodeTables& tables, const int bytes, const uint8_t* target, uint8_t* shown, uint8_t* c0,
                    uint8_t* c1, uint8_t* c2, uint8_t* codes, const int passes) {
  const uint8_t p0 = (passes & 1) ? 0xFF : 0x00;
  const uint8_t p1 = (passes & 2) ? 0xFF : 0x00;
  const uint8_t p2 = (passes & 4) ? 0xFF : 0x00;
  uint8_t pending = 0;
  for (int i = 0; i < bytes; ++i) {
    const uint8_t t = target[i];
    const uint8_t changed = static_cast<uint8_t>(t ^ shown[i]);
    shown[i] = t;
    // Restart changed pixels at `passes`.
    uint8_t b0 = static_cast<uint8_t>((c0[i] & ~changed) | (p0 & changed));
    uint8_t b1 = static_cast<uint8_t>((c1[i] & ~changed) | (p1 & changed));
    uint8_t b2 = static_cast<uint8_t>((c2[i] & ~changed) | (p2 & changed));
    const uint8_t active = static_cast<uint8_t>(b0 | b1 | b2);

    const uint16_t black = tables.code[0][static_cast<uint8_t>(active & ~t)];
    const uint16_t white = tables.code[1][static_cast<uint8_t>(active & t)];
    const uint16_t out = static_cast<uint16_t>(black | white);
    std::memcpy(codes + 2 * i, &out, sizeof(out));

    // Count active pixels down by one (ripple borrow across the three planes).
    const uint8_t borrow0 = static_cast<uint8_t>(active & ~b0);
    b0 = static_cast<uint8_t>(b0 ^ active);
    const uint8_t borrow1 = static_cast<uint8_t>(borrow0 & ~b1);
    b1 = static_cast<uint8_t>(b1 ^ borrow0);
    b2 = static_cast<uint8_t>(b2 ^ borrow1);
    c0[i] = b0;
    c1[i] = b1;
    c2[i] = b2;
    pending |= static_cast<uint8_t>(b0 | b1 | b2);
  }
  return pending != 0;
}

}  // namespace GameVideoRow
