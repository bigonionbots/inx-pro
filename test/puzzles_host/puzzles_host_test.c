/**
 * @file puzzles_host_test.c
 * @brief Host check of the vendored puzzle core (third_party/puzzles) and the puzzles the
 *        firmware offers: Solo (Sudoku).
 *
 * Builds the same wrapper files the firmware does, with a minimal grey raster front end. For each
 * level the firmware offers (src/games/puzzles/PuzzleGames.cpp) it generates puzzles and reports
 * how long that took. Then, per puzzle, it saves and restores a game, solves it and checks that
 * the midend reports it solved; Sudoku also plays a move first. Each last board is written as a
 * PGM image (<prefix>-<puzzle>.pgm, prefix argv[1]) to check the drawing by eye.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define COMBINED
#include "../../third_party/puzzles/puzzles.h"

/* ---- Raster front end (8-bit grey) ---------------------------------------------------------- */

enum { W = 460, H = 460 };
static unsigned char pixels[H][W];
static float* palette;
static int ncolours;
static int clipX0 = 0, clipY0 = 0, clipX1 = W, clipY1 = H;

static unsigned char grey(int colour) {
  const float* c = palette + 3 * colour;
  return (unsigned char)(255.0f * (0.299f * c[0] + 0.587f * c[1] + 0.114f * c[2]));
}

static void plot(int x, int y, unsigned char v) {
  if (x >= clipX0 && x < clipX1 && y >= clipY0 && y < clipY1) pixels[y][x] = v;
}

static void fe_rect(drawing* dr, int x, int y, int w, int h, int colour) {
  (void)dr;
  for (int j = y; j < y + h; j++)
    for (int i = x; i < x + w; i++) plot(i, j, grey(colour));
}

static void fe_line(drawing* dr, int x1, int y1, int x2, int y2, int colour) {
  (void)dr;
  int dx = abs(x2 - x1), dy = -abs(y2 - y1), sx = x1 < x2 ? 1 : -1, sy = y1 < y2 ? 1 : -1, err = dx + dy;
  for (;;) {
    plot(x1, y1, grey(colour));
    if (x1 == x2 && y1 == y2) break;
    int e2 = 2 * err;
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

/* Text: a block per character, solid for clues (black), hollow for other colours. */
static void fe_text(drawing* dr, int x, int y, int fonttype, int fontsize, int align, int colour, const char* text) {
  (void)fonttype;
  int n = (int)strlen(text), cw = fontsize / 2, ch = fontsize * 7 / 10;
  int left = (align & ALIGN_HCENTRE) ? x - n * cw / 2 : (align & ALIGN_HRIGHT) ? x - n * cw : x;
  int top = (align & ALIGN_VCENTRE) ? y - ch / 2 : y - ch;
  for (int k = 0; k < n; k++) {
    int bx = left + k * cw + 1;
    if (grey(colour) < 20) {
      fe_rect(dr, bx, top, cw - 2, ch, colour);
    } else {
      fe_line(dr, bx, top, bx + cw - 3, top, colour);
      fe_line(dr, bx, top + ch - 1, bx + cw - 3, top + ch - 1, colour);
      fe_line(dr, bx, top, bx, top + ch - 1, colour);
      fe_line(dr, bx + cw - 3, top, bx + cw - 3, top + ch - 1, colour);
    }
  }
}

static void fe_polygon(drawing* dr, const int* coords, int npoints, int fill, int outline) {
  for (int i = 0; i < npoints; i++) {
    int j = (i + 1) % npoints;
    fe_line(dr, coords[2 * i], coords[2 * i + 1], coords[2 * j], coords[2 * j + 1], outline);
  }
  (void)fill;
}

static void fe_circle(drawing* dr, int cx, int cy, int r, int fill, int outline) {
  (void)fill;
  fe_rect(dr, cx - r, cy - r, 2 * r, 1, outline);
}

static void fe_update(drawing* dr, int x, int y, int w, int h) {
  (void)dr;
  (void)x;
  (void)y;
  (void)w;
  (void)h;
}
static void fe_clip(drawing* dr, int x, int y, int w, int h) {
  (void)dr;
  clipX0 = x < 0 ? 0 : x;
  clipY0 = y < 0 ? 0 : y;
  clipX1 = x + w > W ? W : x + w;
  clipY1 = y + h > H ? H : y + h;
}
static void fe_unclip(drawing* dr) {
  (void)dr;
  clipX0 = 0;
  clipY0 = 0;
  clipX1 = W;
  clipY1 = H;
}
static void fe_nop(drawing* dr) { (void)dr; }
static void fe_status(drawing* dr, const char* text) {
  (void)dr;
  (void)text;
}
static char* fe_fallback(drawing* dr, const char* const* strings, int n) {
  (void)dr;
  return dupstr(strings[n - 1]);
}

static const drawing_api api = {
    .version = 1,
    .draw_text = fe_text,
    .draw_rect = fe_rect,
    .draw_line = fe_line,
    .draw_polygon = fe_polygon,
    .draw_circle = fe_circle,
    .draw_update = fe_update,
    .clip = fe_clip,
    .unclip = fe_unclip,
    .start_draw = fe_nop,
    .end_draw = fe_nop,
    .status_bar = fe_status,
    .text_fallback = fe_fallback,
};

/* ---- What every front end supplies ---------------------------------------------------------- */

void frontend_default_colour(frontend* fe, float* output) {
  (void)fe;
  output[0] = output[1] = output[2] = 1.0f;
}
void get_random_seed(void** seed, int* size) {
  static unsigned counter;
  unsigned* s = snew(unsigned);
  *s = (unsigned)time(NULL) ^ (++counter * 2654435761u);
  *seed = s;
  *size = sizeof(unsigned);
}
void activate_timer(frontend* fe) { (void)fe; }
void deactivate_timer(frontend* fe) { (void)fe; }
void document_add_puzzle(document* doc, const game* g, game_params* p, game_ui* ui, game_state* s, game_state* s2) {
  (void)doc;
  (void)g;
  (void)p;
  (void)ui;
  (void)s;
  (void)s2;
}
void fatal(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  exit(2);
}

/* ---- Save and restore through memory -------------------------------------------------------- */

struct buf {
  char* data;
  int len, size;
};
static void writeBuf(void* ctx, const void* data, int len) {
  struct buf* b = ctx;
  if (b->len + len > b->size) {
    b->size = (b->len + len) * 2;
    b->data = realloc(b->data, b->size);
  }
  memcpy(b->data + b->len, data, len);
  b->len += len;
}
struct reader {
  struct buf* b;
  int pos;
};
static bool readBuf(void* ctx, void* data, int len) {
  struct reader* r = ctx;
  if (r->pos + len > r->b->len) return false;
  memcpy(data, r->b->data + r->pos, len);
  r->pos += len;
  return true;
}

static double now(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec + ts.tv_nsec / 1e9;
}

struct level {
  const char* name;
  const char* params;
};
struct puzzle {
  const char* name;
  const game* g;
  struct level levels[6];
};

/* Kept in step with the levels in src/games/puzzles/PuzzleGames.cpp. */
static const struct puzzle puzzles[] = {
    {"sudoku",
     &solo,
     {{"Beginner", "3x3dt"}, {"Easy", "3x3db"}, {"Medium", "3x3di"}, {"Hard", "3x3da"}, {"Expert", "3x3de"}}},
};

static midend* newGame(const game* g, const char* encoded) {
  midend* me = midend_new(NULL, g, &api, NULL);
  game_params* params = g->default_params();
  g->decode_params(params, encoded);
  const char* bad = g->validate_params(params, true);
  if (bad) printf("FAIL: %s %s: %s\n", g->name, encoded, bad);
  midend_set_params(me, params);
  g->free_params(params);
  midend_new_game(me);
  return me;
}

/* Draws the board into pixels (and loads the palette the drawing uses). */
static int draw(midend* me) {
  int w = W, h = H;
  midend_size(me, &w, &h, true, 1.0);
  sfree(palette);
  palette = midend_colours(me, &ncolours);
  memset(pixels, 255, sizeof(pixels));
  clipX0 = clipY0 = 0;
  clipX1 = W;
  clipY1 = H;
  midend_force_redraw(me);
  return w;
}

/* Sudoku: select the first empty cell and type 5; that must make a move. */
static bool sudokuMove(midend* me, int w) {
  int tile = w / 9;
  for (int y = 0; y < 9; y++) {
    for (int x = 0; x < 9; x++) {
      midend_process_key(me, x * tile + tile / 2, y * tile + tile / 2, LEFT_BUTTON);
      midend_process_key(me, x * tile + tile / 2, y * tile + tile / 2, LEFT_RELEASE);
      if (midend_process_key(me, 0, 0, '5') && midend_can_undo(me)) return true;
    }
  }
  return false;
}

int main(int argc, char** argv) {
  const int rounds = 5;
  int failures = 0;

  for (size_t p = 0; p < sizeof(puzzles) / sizeof(puzzles[0]); p++) {
    const struct puzzle* pz = &puzzles[p];
    midend* me = NULL;
    for (int l = 0; l < 6 && pz->levels[l].name; l++) {
      double worst = 0, total = 0;
      for (int r = 0; r < rounds; r++) {
        if (me) midend_free(me);
        double t = now();
        me = newGame(pz->g, pz->levels[l].params);
        double took = now() - t;
        total += took;
        if (took > worst) worst = took;
      }
      printf("%-7s %-9s %-6s %7.1f ms average, %7.1f ms worst (%d puzzles)\n", pz->name, pz->levels[l].name,
             pz->levels[l].params, 1000 * total / rounds, 1000 * worst, rounds);
    }

    int w = draw(me);
    if (pz->g == &solo && !sudokuMove(me, w)) {
      printf("FAIL: sudoku: no cell took a digit\n");
      failures++;
    }

    struct buf saved = {0};
    midend_serialise(me, writeBuf, &saved);
    char* idBefore = midend_get_game_id(me);
    midend* restored = midend_new(NULL, pz->g, &api, NULL);
    struct reader rd = {&saved, 0};
    const char* err = midend_deserialise(restored, readBuf, &rd);
    char* idAfter = midend_get_game_id(restored);
    if (err || strcmp(idBefore, idAfter) != 0 || (pz->g == &solo && !midend_can_undo(restored))) {
      printf("FAIL: %s: save and restore (%s)\n", pz->name, err ? err : "different game or lost the move");
      failures++;
    } else {
      printf("%s: save and restore, %d bytes\n", pz->name, saved.len);
    }

    err = midend_solve(restored);
    if (err || midend_status(restored) != 1) {
      printf("FAIL: %s: solve (%s, status %d)\n", pz->name, err ? err : "no error", midend_status(restored));
      failures++;
    } else {
      printf("%s: solve completed\n", pz->name);
    }

    if (argc > 1) {
      draw(restored);
      char path[512];
      snprintf(path, sizeof(path), "%s-%s.pgm", argv[1], pz->name);
      FILE* f = fopen(path, "wb");
      if (f) {
        fprintf(f, "P5\n%d %d\n255\n", W, H);
        fwrite(pixels, 1, sizeof(pixels), f);
        fclose(f);
      }
    }
    sfree(idBefore);
    sfree(idAfter);
    free(saved.data);
    midend_free(restored);
    midend_free(me);
  }
  sfree(palette);
  printf(failures ? "FAILED\n" : "OK\n");
  return failures ? 1 : 0;
}
