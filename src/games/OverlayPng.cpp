/**
 * @file OverlayPng.cpp
 * @brief Definitions for the overlay PNG decoder.
 */

#include "OverlayPng.h"

#if INX_HAS_GAMES || defined(GB_HOST_TEST)

#include <InflateReader.h>

#include <cstdlib>
#include <cstring>

namespace OverlayPng {

namespace {

constexpr uint8_t SIGNATURE[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
constexpr uint8_t BAYER4[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};

uint32_t be32(const uint8_t* p) {
  return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) | p[3];
}

uint8_t paeth(const int a, const int b, const int c) {
  const int p = a + b - c;
  const int pa = std::abs(p - a);
  const int pb = std::abs(p - b);
  const int pc = std::abs(p - c);
  if (pa <= pb && pa <= pc) return static_cast<uint8_t>(a);
  return static_cast<uint8_t>(pb <= pc ? b : c);
}

/** Undoes one row's filter in place. `prev` is the unfiltered row above (zeros for the first). */
bool unfilter(const uint8_t type, uint8_t* row, const uint8_t* prev, const size_t stride, const size_t bpp) {
  switch (type) {
    case 0:
      return true;
    case 1:
      for (size_t i = bpp; i < stride; ++i) row[i] = static_cast<uint8_t>(row[i] + row[i - bpp]);
      return true;
    case 2:
      for (size_t i = 0; i < stride; ++i) row[i] = static_cast<uint8_t>(row[i] + prev[i]);
      return true;
    case 3:
      for (size_t i = 0; i < stride; ++i) {
        const int left = i >= bpp ? row[i - bpp] : 0;
        row[i] = static_cast<uint8_t>(row[i] + ((left + prev[i]) >> 1));
      }
      return true;
    case 4:
      for (size_t i = 0; i < stride; ++i) {
        const int left = i >= bpp ? row[i - bpp] : 0;
        const int upLeft = i >= bpp ? prev[i - bpp] : 0;
        row[i] = static_cast<uint8_t>(row[i] + paeth(left, prev[i], upLeft));
      }
      return true;
    default:
      return false;
  }
}

struct Format {
  uint8_t colorType = 0;
  uint8_t depth = 8;
  uint8_t palette[256][3] = {};
  uint8_t paletteAlpha[256];
  bool hasKey = false;  // tRNS colour key for grey / RGB
  uint16_t key[3] = {};

  Format() { std::memset(paletteAlpha, 255, sizeof(paletteAlpha)); }

  int channels() const {
    switch (colorType) {
      case 2:
        return 3;
      case 4:
        return 2;
      case 6:
        return 4;
      default:
        return 1;
    }
  }
};

/** Sample `index` (0-based) of a row of `depth`-bit samples, scaled to 0..255 unless `raw`. */
uint16_t sample(const uint8_t* row, const size_t index, const uint8_t depth, const bool raw) {
  switch (depth) {
    case 16:
      return raw ? static_cast<uint16_t>((row[2 * index] << 8) | row[2 * index + 1]) : row[2 * index];
    case 8:
      return row[index];
    default: {
      const size_t bit = index * depth;
      const int shift = 8 - depth - static_cast<int>(bit % 8);
      const uint16_t v = static_cast<uint16_t>((row[bit / 8] >> shift) & ((1 << depth) - 1));
      return raw ? v : static_cast<uint16_t>(v * 255 / ((1 << depth) - 1));
    }
  }
}

void pixel(const Format& f, const uint8_t* row, const int x, uint8_t& lum, uint8_t& alpha) {
  const size_t base = static_cast<size_t>(x) * f.channels();
  alpha = 255;
  switch (f.colorType) {
    case 0: {
      lum = static_cast<uint8_t>(sample(row, base, f.depth, false));
      if (f.hasKey && sample(row, base, f.depth, true) == f.key[0]) alpha = 0;
      break;
    }
    case 2: {
      const uint16_t r = sample(row, base, f.depth, false);
      const uint16_t g = sample(row, base + 1, f.depth, false);
      const uint16_t b = sample(row, base + 2, f.depth, false);
      lum = static_cast<uint8_t>((r * 77 + g * 150 + b * 29) >> 8);
      if (f.hasKey && sample(row, base, f.depth, true) == f.key[0] &&
          sample(row, base + 1, f.depth, true) == f.key[1] && sample(row, base + 2, f.depth, true) == f.key[2]) {
        alpha = 0;
      }
      break;
    }
    case 3: {
      const uint16_t i = sample(row, base, f.depth, true);
      lum = static_cast<uint8_t>((f.palette[i][0] * 77 + f.palette[i][1] * 150 + f.palette[i][2] * 29) >> 8);
      alpha = f.paletteAlpha[i];
      break;
    }
    case 4:
      lum = static_cast<uint8_t>(sample(row, base, f.depth, false));
      alpha = static_cast<uint8_t>(sample(row, base + 1, f.depth, false));
      break;
    case 6:
    default: {
      const uint16_t r = sample(row, base, f.depth, false);
      const uint16_t g = sample(row, base + 1, f.depth, false);
      const uint16_t b = sample(row, base + 2, f.depth, false);
      lum = static_cast<uint8_t>((r * 77 + g * 150 + b * 29) >> 8);
      alpha = static_cast<uint8_t>(sample(row, base + 3, f.depth, false));
      break;
    }
  }
}

bool validFormat(const uint8_t colorType, const uint8_t depth) {
  switch (colorType) {
    case 0:
      return depth == 1 || depth == 2 || depth == 4 || depth == 8 || depth == 16;
    case 3:
      return depth == 1 || depth == 2 || depth == 4 || depth == 8;
    case 2:
    case 4:
    case 6:
      return depth == 8 || depth == 16;
    default:
      return false;
  }
}

}  // namespace

const char* describe(const Error error) {
  switch (error) {
    case Error::None:
      return "ok";
    case Error::NotPng:
      return "not a PNG file";
    case Error::WrongSize:
      return "must be 960 x 540";
    case Error::Unsupported:
      return "unsupported PNG (interlaced?)";
    case Error::Corrupt:
      return "damaged PNG";
    case Error::OutOfMemory:
    default:
      return "out of memory";
  }
}

Error decode(const uint8_t* png, const size_t length, const int width, const int height, uint8_t* ink,
             uint8_t* opaque) {
  if (length < 8 + 25 || std::memcmp(png, SIGNATURE, sizeof(SIGNATURE)) != 0) return Error::NotPng;

  Format format;
  size_t pos = 8;
  bool haveHeader = false;
  size_t idatTotal = 0;
  // First pass: header, palette, transparency, and the total size of the image data.
  while (pos + 12 <= length) {
    const uint32_t chunkLength = be32(png + pos);
    const uint8_t* type = png + pos + 4;
    const uint8_t* data = png + pos + 8;
    if (chunkLength > length - pos - 12) return Error::Corrupt;
    if (std::memcmp(type, "IHDR", 4) == 0) {
      if (chunkLength < 13) return Error::Corrupt;
      if (static_cast<int>(be32(data)) != width || static_cast<int>(be32(data + 4)) != height) return Error::WrongSize;
      format.depth = data[8];
      format.colorType = data[9];
      if (data[10] != 0 || data[11] != 0 || data[12] != 0) return Error::Unsupported;  // compression, filter, interlace
      if (!validFormat(format.colorType, format.depth)) return Error::Unsupported;
      haveHeader = true;
    } else if (std::memcmp(type, "PLTE", 4) == 0) {
      for (uint32_t i = 0; i < chunkLength / 3 && i < 256; ++i) std::memcpy(format.palette[i], data + 3 * i, 3);
    } else if (std::memcmp(type, "tRNS", 4) == 0) {
      if (format.colorType == 3) {
        for (uint32_t i = 0; i < chunkLength && i < 256; ++i) format.paletteAlpha[i] = data[i];
      } else if (format.colorType == 0 && chunkLength >= 2) {
        format.hasKey = true;
        format.key[0] = static_cast<uint16_t>((data[0] << 8) | data[1]);
      } else if (format.colorType == 2 && chunkLength >= 6) {
        format.hasKey = true;
        for (int c = 0; c < 3; ++c) format.key[c] = static_cast<uint16_t>((data[2 * c] << 8) | data[2 * c + 1]);
      }
    } else if (std::memcmp(type, "IDAT", 4) == 0) {
      idatTotal += chunkLength;
    } else if (std::memcmp(type, "IEND", 4) == 0) {
      break;
    }
    pos += 12 + chunkLength;
  }
  if (!haveHeader || idatTotal < 2) return Error::Corrupt;

  // Gather the image data into one buffer and inflate it in one go.
  auto* compressed = static_cast<uint8_t*>(std::malloc(idatTotal));
  const size_t bitsPerPixel = static_cast<size_t>(format.channels()) * format.depth;
  const size_t stride = (static_cast<size_t>(width) * bitsPerPixel + 7) / 8;
  const size_t rawSize = static_cast<size_t>(height) * (stride + 1);
  auto* raw = static_cast<uint8_t*>(std::malloc(rawSize));
  auto* zeros = static_cast<uint8_t*>(std::calloc(1, stride));
  if (!compressed || !raw || !zeros) {
    std::free(compressed);
    std::free(raw);
    std::free(zeros);
    return Error::OutOfMemory;
  }
  size_t filled = 0;
  for (pos = 8; pos + 12 <= length;) {
    const uint32_t chunkLength = be32(png + pos);
    if (std::memcmp(png + pos + 4, "IDAT", 4) == 0) {
      std::memcpy(compressed + filled, png + pos + 8, chunkLength);
      filled += chunkLength;
    }
    pos += 12 + chunkLength;
  }
  InflateReader inflater;
  bool ok = inflater.init(false);
  if (ok) {
    inflater.setSource(compressed, idatTotal);
    inflater.skipZlibHeader();
    ok = inflater.read(raw, rawSize);
  }
  inflater.deinit();
  std::free(compressed);

  Error result = ok ? Error::None : Error::Corrupt;
  const size_t bpp = bitsPerPixel >= 8 ? bitsPerPixel / 8 : 1;
  const size_t planeRow = static_cast<size_t>(width) / 8;
  for (int y = 0; y < height && result == Error::None; ++y) {
    uint8_t* row = raw + static_cast<size_t>(y) * (stride + 1);
    const uint8_t* prev = y > 0 ? raw + static_cast<size_t>(y - 1) * (stride + 1) + 1 : zeros;
    if (!unfilter(row[0], row + 1, prev, stride, bpp)) {
      result = Error::Corrupt;
      break;
    }
    uint8_t* inkRow = ink + static_cast<size_t>(y) * planeRow;
    uint8_t* opaqueRow = opaque + static_cast<size_t>(y) * planeRow;
    std::memset(inkRow, 0xFF, planeRow);
    std::memset(opaqueRow, 0, planeRow);
    for (int x = 0; x < width; ++x) {
      uint8_t lum = 0;
      uint8_t alpha = 0;
      pixel(format, row + 1, x, lum, alpha);
      const uint8_t bit = static_cast<uint8_t>(0x80 >> (x & 7));
      if (alpha >= 128) opaqueRow[x / 8] |= bit;
      if (lum < BAYER4[y & 3][x & 3] * 16 + 8) inkRow[x / 8] &= static_cast<uint8_t>(~bit);
    }
  }
  std::free(raw);
  std::free(zeros);
  return result;
}

}  // namespace OverlayPng

#endif  // INX_HAS_GAMES || GB_HOST_TEST
