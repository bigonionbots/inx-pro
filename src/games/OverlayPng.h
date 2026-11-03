#pragma once

/**
 * @file OverlayPng.h
 * @brief Decodes a game-screen overlay PNG into two 1-bit planes: what to draw, and where.
 *
 * Overlays are drawn over the game-mode screen (GameOverlay.h). A pixel with alpha of at
 * least 50% is opaque and replaces the screen; anything more transparent leaves the screen as
 * it is. Opaque pixels become black or white with a 4x4 ordered dither of their brightness, so
 * greys come out as patterns.
 *
 * Supports non-interlaced PNGs of every colour type (grey, RGB, palette, grey + alpha, RGBA)
 * at 8 bits per channel, palettes at 1, 2, 4 or 8 bits, and tRNS transparency. 16-bit channels
 * use their high byte. Plain C++ over a memory buffer, so the host tests can run it.
 */

#include <cstddef>
#include <cstdint>

namespace OverlayPng {

/** Why decode() failed, for the log and the screen. */
enum class Error : uint8_t { None, NotPng, WrongSize, Unsupported, Corrupt, OutOfMemory };

const char* describe(Error error);

/**
 * Decodes `png` into `ink` and `opaque`, each width * height / 8 bytes, 1 bit per pixel, MSB =
 * leftmost. In `ink` 1 = white, as in FastEPD's 1-bpp buffer; in `opaque` 1 = drawn. The PNG
 * must be exactly width x height.
 */
Error decode(const uint8_t* png, size_t length, int width, int height, uint8_t* ink, uint8_t* opaque);

}  // namespace OverlayPng
