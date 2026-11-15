/**
 * @file solo_host_test.c
 * @brief Host check of the vendored puzzle core (third_party/puzzles) with Solo (Sudoku).
 *
 * Builds the same wrapper files the firmware does, with a minimal grey raster front end. For each
 * difficulty the firmware offers it generates puzzles and reports how long that took, then plays
 * a move, saves and restores the game, solves it, and checks that the midend reports it solved.
 * The last board is written as a PGM image (argv[1]) to check the drawing by eye.
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

int main(int argc, char** argv) {
  /* The difficulties the firmware offers (SudokuActivity), as Solo's parameter strings. */
  const char* levels[] = {"3x3dt", "3x3db", "3x3di", "3x3da", "3x3de"};
  const char* names[] = {"Beginner", "Easy", "Medium", "Hard", "Expert"};
  const int rounds = 5;
  int failures = 0;

  midend* me = NULL;
  for (int l = 0; l < 5; l++) {
    double worst = 0, total = 0;
    for (int r = 0; r < rounds; r++) {
      if (me) midend_free(me);
      me = midend_new(NULL, &solo, &api, NULL);
      game_params* params = solo.default_params();
      solo.decode_params(params, levels[l]);
      midend_set_params(me, params);
      solo.free_params(params);
      double t = now();
      midend_new_game(me);
      double took = now() - t;
      total += took;
      if (took > worst) worst = took;
    }
    printf("%-9s %s: %.1f ms average, %.1f ms worst (%d puzzles)\n", names[l], levels[l], 1000 * total / rounds,
           1000 * worst, rounds);
  }

  /* Draw, select a cell and enter a digit, then save and restore. */
  int w = W, h = H;
  midend_size(me, &w, &h, true, 1.0);
  palette = midend_colours(me, &ncolours);
  memset(pixels, 255, sizeof(pixels));
  midend_force_redraw(me);
  int tile = w / 9;
  for (int y = 0; y < 9; y++) {
    for (int x = 0; x < 9; x++) {
      /* the first empty cell: clicking it and typing 5 must make a move */
      midend_process_key(me, x * tile + tile / 2, y * tile + tile / 2, LEFT_BUTTON);
      midend_process_key(me, x * tile + tile / 2, y * tile + tile / 2, LEFT_RELEASE);
      if (midend_process_key(me, 0, 0, '5') && midend_can_undo(me)) goto moved;
    }
  }
  printf("FAIL: no cell took a digit\n");
  failures++;
moved:;
  struct buf saved = {0};
  midend_serialise(me, writeBuf, &saved);
  char* idBefore = midend_get_game_id(me);
  midend* restored = midend_new(NULL, &solo, &api, NULL);
  struct reader rd = {&saved, 0};
  const char* err = midend_deserialise(restored, readBuf, &rd);
  char* idAfter = midend_get_game_id(restored);
  if (err || strcmp(idBefore, idAfter) != 0 || !midend_can_undo(restored)) {
    printf("FAIL: save and restore (%s)\n", err ? err : "different game or lost the move");
    failures++;
  } else {
    printf("save and restore: %d bytes, same game, move kept\n", saved.len);
  }

  /* Solve: the midend must report the puzzle completed. */
  err = midend_solve(restored);
  if (err || midend_status(restored) != 1) {
    printf("FAIL: solve (%s, status %d)\n", err ? err : "no error", midend_status(restored));
    failures++;
  } else {
    printf("solve: completed\n");
  }

  if (argc > 1) {
    FILE* f = fopen(argv[1], "wb");
    fprintf(f, "P5\n%d %d\n255\n", W, H);
    fwrite(pixels, 1, sizeof(pixels), f);
    fclose(f);
  }
  sfree(idBefore);
  sfree(idAfter);
  free(saved.data);
  sfree(palette);
  midend_free(restored);
  midend_free(me);
  printf(failures ? "FAILED\n" : "OK\n");
  return failures ? 1 : 0;
}
