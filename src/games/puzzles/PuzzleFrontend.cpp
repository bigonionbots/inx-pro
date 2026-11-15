/**
 * @file PuzzleFrontend.cpp
 * @brief Definitions for PuzzleFrontend, and the C functions the puzzle collection expects of
 *        every front end.
 */

#include "PuzzleFrontend.h"

#if INX_HAS_PUZZLES

#include <Arduino.h>
#include <SDCardManager.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "system/Fonts.h"
#include "util/SdIoMutex.h"

#ifndef SIMULATOR
#include <esp_random.h>
#endif

extern "C" {
#define COMBINED
#include "../../../third_party/puzzles/puzzles.h"
#undef COMBINED
}
// puzzles.h defines max() and min() as macros, which would break std::max and std::min.
#undef max
#undef min

namespace {

PuzzleFrontend* from(drawing* dr) { return GET_HANDLE_AS_TYPE(dr, PuzzleFrontend); }

void cbText(drawing* dr, int x, int y, int /*fonttype*/, int fontsize, int align, int colour, const char* text) {
  from(dr)->drawText(x, y, fontsize, align, colour, text);
}
void cbRect(drawing* dr, int x, int y, int w, int h, int colour) { from(dr)->drawRect(x, y, w, h, colour); }
void cbLine(drawing* dr, int x1, int y1, int x2, int y2, int colour) { from(dr)->drawLine(x1, y1, x2, y2, colour); }
void cbPolygon(drawing* dr, const int* coords, int npoints, int fill, int outline) {
  from(dr)->drawPolygon(coords, npoints, fill, outline);
}
void cbCircle(drawing* dr, int cx, int cy, int radius, int fill, int outline) {
  from(dr)->drawCircle(cx, cy, radius, fill, outline);
}
void cbUpdate(drawing* dr, int x, int y, int w, int h) { from(dr)->markUpdate(x, y, w, h); }
void cbClip(drawing* dr, int x, int y, int w, int h) { from(dr)->setClip(x, y, w, h); }
void cbUnclip(drawing* dr) { from(dr)->clearClip(); }
void cbNothing(drawing*) {}
void cbStatus(drawing*, const char*) {}
/** The collection offers a fancy string and plainer fallbacks: take the first plain-ASCII one. */
char* cbFallback(drawing*, const char* const* strings, int count) {
  for (int i = 0; i < count; ++i) {
    bool ascii = true;
    for (const char* c = strings[i]; *c; ++c) ascii = ascii && static_cast<unsigned char>(*c) < 0x80;
    if (ascii) return dupstr(strings[i]);
  }
  return dupstr(strings[count - 1]);
}

drawing_api makeApi() {
  drawing_api api = {};
  api.version = 1;
  api.draw_text = cbText;
  api.draw_rect = cbRect;
  api.draw_line = cbLine;
  api.draw_polygon = cbPolygon;
  api.draw_circle = cbCircle;
  api.draw_update = cbUpdate;
  api.clip = cbClip;
  api.unclip = cbUnclip;
  api.start_draw = cbNothing;
  api.end_draw = cbNothing;
  api.status_bar = cbStatus;
  api.text_fallback = cbFallback;
  // No blitters (none of the vendored puzzles use them) and no printing.
  return api;
}
const drawing_api kApi = makeApi();

/** The front end handle the midend passes to the timer calls; see PuzzleFrontend's constructor. */
PuzzleFrontend* timerOwner(frontend* fe) { return reinterpret_cast<PuzzleFrontend*>(fe); }

/** Digits look right with their glyph height at about 70% of the collection's font size. */
constexpr int kDigitHeightPct = 70;
constexpr int kFonts[] = {MONTSERRAT_8_FONT_ID,  MONTSERRAT_10_FONT_ID, MONTSERRAT_12_FONT_ID,
                          MONTSERRAT_14_FONT_ID, MONTSERRAT_16_FONT_ID, MONTSERRAT_18_FONT_ID};

}  // namespace

// ---- What every front end supplies (puzzles.h) ----------------------------------------------

extern "C" {

void frontend_default_colour(frontend*, float* output) { output[0] = output[1] = output[2] = 1.0f; }

void get_random_seed(void** seed, int* size) {
  auto* bytes = snewn(16, unsigned char);
  for (int i = 0; i < 16; i += 4) {
#ifdef SIMULATOR
    const uint32_t r = static_cast<uint32_t>(rand()) ^ static_cast<uint32_t>(millis() << 8);
#else
    const uint32_t r = esp_random();
#endif
    std::memcpy(bytes + i, &r, 4);
  }
  *seed = bytes;
  *size = 16;
}

void activate_timer(frontend* fe) {
  if (fe) timerOwner(fe)->timerOn();
}
void deactivate_timer(frontend* fe) {
  if (fe) timerOwner(fe)->timerOff();
}

void document_add_puzzle(document*, const game*, game_params*, game_ui*, game_state*, game_state*) {}

void fatal(const char* fmt, ...) {
  char text[160];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(text, sizeof(text), fmt, ap);
  va_end(ap);
  INX_SERIAL.printf("[PUZZLE] fatal: %s\n", text);
  abort();
}

}  // extern "C"

// ---- PuzzleFrontend --------------------------------------------------------------------------

PuzzleFrontend::PuzzleFrontend(GfxRenderer& renderer, const game& puzzle) : renderer(renderer), puzzle(puzzle) {
  // The midend hands `frontend*` back only to the timer calls; it never looks inside.
  me = midend_new(reinterpret_cast<frontend*>(this), &puzzle, &kApi, this);
}

PuzzleFrontend::~PuzzleFrontend() {
  sfree(palette);
  midend_free(me);
}

void PuzzleFrontend::loadPalette() {
  sfree(palette);
  palette = midend_colours(me, &colours);
}

void PuzzleFrontend::layout(const int x, const int y, const int width, const int height) {
  int w = width;
  int h = height;
  midend_size(me, &w, &h, true, 1.0);  // the largest board that fits; w and h become its size
  boardW = w;
  boardH = h;
  boardX = x + (width - w) / 2;
  boardY = y + (height - h) / 2;
  if (!palette) loadPalette();
}

bool PuzzleFrontend::setParams(const std::string& encoded) {
  game_params* p = puzzle.default_params();
  puzzle.decode_params(p, encoded.c_str());
  const bool ok = puzzle.validate_params(p, true) == nullptr;
  if (ok) midend_set_params(me, p);
  puzzle.free_params(p);
  return ok;
}

std::string PuzzleFrontend::params() const {
  game_params* p = midend_get_params(me);
  char* text = puzzle.encode_params(p, true);
  std::string result = text;
  sfree(text);
  puzzle.free_params(p);
  return result;
}

void PuzzleFrontend::newGame() {
  midend_new_game(me);
  loadPalette();
}

namespace {
void appendTo(void* ctx, const void* data, int length) {
  static_cast<std::string*>(ctx)->append(static_cast<const char*>(data), static_cast<size_t>(length));
}
struct Reader {
  const std::string* text;
  size_t pos;
};
bool readFrom(void* ctx, void* data, int length) {
  auto* r = static_cast<Reader*>(ctx);
  if (r->pos + static_cast<size_t>(length) > r->text->size()) return false;
  std::memcpy(data, r->text->data() + r->pos, static_cast<size_t>(length));
  r->pos += static_cast<size_t>(length);
  return true;
}
}  // namespace

bool PuzzleFrontend::save(const char* path) const {
  std::string text;
  midend_serialise(me, appendTo, &text);
  SdIoMutex::Lock lock;
  FsFile file = SdMan.open(path, O_WRONLY | O_CREAT | O_TRUNC);
  if (!file) return false;
  const bool ok = file.write(text.data(), text.size()) == text.size();
  file.close();
  return ok;
}

void PuzzleFrontend::restart() { midend_restart_game(me); }

bool PuzzleFrontend::load(const char* path) {
  std::string text;
  {
    SdIoMutex::Lock lock;
    FsFile file = SdMan.open(path, O_RDONLY);
    if (!file) return false;
    char chunk[256];
    for (int n; (n = file.read(chunk, sizeof(chunk))) > 0;) text.append(chunk, static_cast<size_t>(n));
    file.close();
  }
  Reader reader{&text, 0};
  const char* error = midend_deserialise(me, readFrom, &reader);
  if (error) {
    INX_SERIAL.printf("[PUZZLE] %s: %s\n", path, error);
    return false;
  }
  loadPalette();
  return true;
}

void PuzzleFrontend::drawAll() {
  clearClip();
  midend_force_redraw(me);
  markUpdate(0, 0, boardW, boardH);
}

bool PuzzleFrontend::tap(const int screenX, const int screenY, const bool alternate) {
  const int x = screenX - boardX;
  const int y = screenY - boardY;
  if (x < 0 || y < 0 || x >= boardW || y >= boardH) return false;
  const bool used = midend_process_key(me, x, y, alternate ? RIGHT_BUTTON : LEFT_BUTTON) != PKR_UNUSED;
  midend_process_key(me, x, y, alternate ? RIGHT_RELEASE : LEFT_RELEASE);
  return used;
}

bool PuzzleFrontend::key(const int button) { return midend_process_key(me, 0, 0, button) != PKR_UNUSED; }

bool PuzzleFrontend::canUndo() const { return midend_can_undo(me); }
bool PuzzleFrontend::canRedo() const { return midend_can_redo(me); }
int PuzzleFrontend::status() const { return midend_status(me); }

void PuzzleFrontend::service() {
  // A move's animation or the completion flash: jump to its end, one redraw instead of many.
  if (timerActive) midend_timer(me, 60.0f);
}

bool PuzzleFrontend::takeDirty(GfxRenderer::DisplayRegion& changed) {
  if (!dirty) return false;
  dirty = false;
  changed = {dirtyX0, dirtyY0, dirtyX1 - dirtyX0, dirtyY1 - dirtyY0};
  return true;
}

// ---- Drawing ---------------------------------------------------------------------------------

PuzzleFrontend::Tone PuzzleFrontend::tone(const int colour) const {
  if (!palette || colour < 0 || colour >= colours) return Tone::Ink;
  const float* c = palette + 3 * colour;
  const float lum = 0.299f * c[0] + 0.587f * c[1] + 0.114f * c[2];
  if (lum >= 0.85f) return Tone::Paper;
  if (lum >= 0.45f) return Tone::Gray;
  return Tone::Ink;
}

PuzzleFrontend::TextStyle PuzzleFrontend::textStyle(const int colour) const {
  if (!palette || colour < 0 || colour >= colours) return TextStyle::Bold;
  const float* c = palette + 3 * colour;
  if (c[0] > 0.6f && c[1] < 0.4f && c[2] < 0.4f) return TextStyle::Error;
  if (c[0] + c[1] + c[2] < 0.05f) return TextStyle::Bold;
  return TextStyle::Regular;
}

void PuzzleFrontend::markUpdate(const int x, const int y, const int w, const int h) {
  const int x0 = std::max(0, x) + boardX;
  const int y0 = std::max(0, y) + boardY;
  const int x1 = std::min(boardW, x + w) + boardX;
  const int y1 = std::min(boardH, y + h) + boardY;
  if (x1 <= x0 || y1 <= y0) return;
  if (!dirty) {
    dirtyX0 = x0;
    dirtyY0 = y0;
    dirtyX1 = x1;
    dirtyY1 = y1;
    dirty = true;
  } else {
    dirtyX0 = std::min(dirtyX0, x0);
    dirtyY0 = std::min(dirtyY0, y0);
    dirtyX1 = std::max(dirtyX1, x1);
    dirtyY1 = std::max(dirtyY1, y1);
  }
}

void PuzzleFrontend::setClip(const int x, const int y, const int w, const int h) {
  clipX0 = std::max(0, x);
  clipY0 = std::max(0, y);
  clipX1 = std::min(boardW, x + w);
  clipY1 = std::min(boardH, y + h);
  clipping = true;
}

void PuzzleFrontend::clearClip() { clipping = false; }

void PuzzleFrontend::plot(const int x, const int y, const Tone t) {
  if (x < 0 || y < 0 || x >= boardW || y >= boardH) return;
  if (clipping && (x < clipX0 || y < clipY0 || x >= clipX1 || y >= clipY1)) return;
  const int sx = boardX + x;
  const int sy = boardY + y;
  renderer.drawPixel(sx, sy, t == Tone::Ink || (t == Tone::Gray && ((sx + sy) & 1) == 0));
}

void PuzzleFrontend::fill(int x, int y, int w, int h, const Tone t) {
  int x1 = std::min(boardW, x + w);
  int y1 = std::min(boardH, y + h);
  x = std::max(0, x);
  y = std::max(0, y);
  if (clipping) {
    x = std::max(x, clipX0);
    y = std::max(y, clipY0);
    x1 = std::min(x1, clipX1);
    y1 = std::min(y1, clipY1);
  }
  if (x1 <= x || y1 <= y) return;
  if (t == Tone::Gray) {
    for (int py = y; py < y1; ++py)
      for (int px = x; px < x1; ++px) plot(px, py, t);
    return;
  }
  renderer.rectangle.fill(boardX + x, boardY + y, x1 - x, y1 - y, t == Tone::Ink);
}

void PuzzleFrontend::line(int x1, int y1, const int x2, const int y2, const Tone t) {
  const int dx = std::abs(x2 - x1);
  const int dy = -std::abs(y2 - y1);
  const int sx = x1 < x2 ? 1 : -1;
  const int sy = y1 < y2 ? 1 : -1;
  int err = dx + dy;
  for (;;) {
    plot(x1, y1, t == Tone::Gray ? Tone::Ink : t);  // a dithered one-pixel line all but vanishes
    if (x1 == x2 && y1 == y2) break;
    const int e2 = 2 * err;
    if (e2 >= dy) {
      err += dy;
      x1 += sx;
    }
    if (e2 <= dx) {
      err += dx;
      y1 += sy;
    }
  }
}

void PuzzleFrontend::drawRect(const int x, const int y, const int w, const int h, const int colour) {
  fill(x, y, w, h, tone(colour));
}

void PuzzleFrontend::drawLine(const int x1, const int y1, const int x2, const int y2, const int colour) {
  line(x1, y1, x2, y2, tone(colour));
}

void PuzzleFrontend::drawPolygon(const int* coords, const int points, const int fillColour, const int outline) {
  if (points < 2) return;
  if (fillColour >= 0) {
    // Even-odd scanline fill over the polygon's bounding box.
    int minY = coords[1], maxY = coords[1];
    for (int i = 1; i < points; ++i) {
      minY = std::min(minY, coords[2 * i + 1]);
      maxY = std::max(maxY, coords[2 * i + 1]);
    }
    const Tone t = tone(fillColour);
    for (int y = minY; y <= maxY; ++y) {
      int xs[16];
      int n = 0;
      for (int i = 0; i < points && n < 16; ++i) {
        const int j = (i + 1) % points;
        const int ax = coords[2 * i], ay = coords[2 * i + 1];
        const int bx = coords[2 * j], by = coords[2 * j + 1];
        if ((ay <= y && by > y) || (by <= y && ay > y)) xs[n++] = ax + (y - ay) * (bx - ax) / (by - ay);
      }
      std::sort(xs, xs + n);
      for (int k = 0; k + 1 < n; k += 2) fill(xs[k], y, xs[k + 1] - xs[k] + 1, 1, t);
    }
  }
  const Tone edge = tone(outline);
  for (int i = 0; i < points; ++i) {
    const int j = (i + 1) % points;
    line(coords[2 * i], coords[2 * i + 1], coords[2 * j], coords[2 * j + 1], edge);
  }
}

void PuzzleFrontend::drawCircle(const int cx, const int cy, const int radius, const int fillColour, const int outline) {
  if (fillColour >= 0) {
    const Tone t = tone(fillColour);
    for (int dy = -radius; dy <= radius; ++dy) {
      int dx = 0;
      while ((dx + 1) * (dx + 1) + dy * dy <= radius * radius) ++dx;
      fill(cx - dx, cy + dy, 2 * dx + 1, 1, t);
    }
  }
  const Tone edge = tone(outline);
  int x = radius, y = 0, err = 1 - radius;
  while (x >= y) {
    const int pts[8][2] = {{x, y}, {y, x}, {-y, x}, {-x, y}, {-x, -y}, {-y, -x}, {y, -x}, {x, -y}};
    for (const auto& p : pts) plot(cx + p[0], cy + p[1], edge);
    ++y;
    if (err < 0) {
      err += 2 * y + 1;
    } else {
      --x;
      err += 2 * (y - x) + 1;
    }
  }
}

void PuzzleFrontend::drawText(const int x, const int y, const int fontSize, const int align, const int colour,
                              const char* text) {
  if (!text || !*text) return;
  const TextStyle style = textStyle(colour);
  const EpdFontFamily::Style weight = style == TextStyle::Bold ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;

  // The largest built-in size whose digits are close to the wanted height, else the largest scaled.
  const int wanted = std::max(4, fontSize * kDigitHeightPct / 100);
  const auto digitHeight = [&](const int font) {
    return renderer.text.getFontAscenderSize(font) - renderer.text.getGlyphTopInset(font, '8', weight);
  };
  int font = kFonts[0];
  int scale = 100;
  for (const int candidate : kFonts) {
    if (digitHeight(candidate) <= wanted) font = candidate;
  }
  if (digitHeight(font) < wanted * 85 / 100) {
    font = kFonts[sizeof(kFonts) / sizeof(kFonts[0]) - 1];
    scale = std::clamp(wanted * 100 / std::max(1, digitHeight(font)), 50, 250);
  }
  const int ascender = renderer.text.getFontAscenderSize(font) * scale / 100;
  const int topInset = renderer.text.getGlyphTopInset(font, '8', weight) * scale / 100;
  const int width = scale == 100 ? renderer.text.getUntranslatedWidth(font, text, weight)
                                 : renderer.text.getScaledWidth(font, text, static_cast<uint8_t>(scale), weight);

  int left = x;
  if (align & ALIGN_HCENTRE) left -= width / 2;
  if (align & ALIGN_HRIGHT) left -= width;
  // The renderer's y is the top of the line box (baseline = y + ascender).
  int top = (align & ALIGN_VCENTRE) ? y - (ascender + topInset) / 2 : y - ascender;
  left += boardX;
  top += boardY;

  const bool black = style != TextStyle::Error;
  if (style == TextStyle::Error) {
    const int pad = std::max(2, fontSize / 10);
    renderer.rectangle.fill(left - pad, top + topInset - pad, width + 2 * pad, ascender - topInset + 2 * pad, true);
  }
  if (scale == 100) {
    renderer.text.renderUntranslated(font, left, top, text, black, weight);
  } else {
    renderer.text.renderScaled(font, left, top, text, static_cast<uint8_t>(scale), black, weight);
  }
}

#endif  // INX_HAS_PUZZLES
