/**
 * @file GbPort.c
 * @brief Globals and helpers the vendored CrankBoy core expects from its Playdate app.
 *
 * See third_party/crankboy/src/crankboy_shim.h for the declarations. The values here pick
 * the plain path through the core: no ITCM relocation, no CrankBoy CGB contrast modes, fast
 * (non-accurate) sound. Builds on the device (t5s3_audio) and on the host for the core tests.
 */

#if INX_HAS_GAMES || defined(GB_HOST_TEST)

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../../third_party/crankboy/libs/peanut_gb.h"

#ifdef ARDUINO
#include <esp_heap_caps.h>
#endif

/* ---- Preferences ---- */
int preferences_sound_mode = 1;
int preferences_sample_rate = 0;
int preferences_high_pass_filter = 1;
int preferences_uncap_fps = 0;
int preferences_cgb_speed = 0;
int preferences_cgb_gamma = 0;
int preferences_cgb_bias_auto = 0;
int preferences_dither_line = 0;
int preferences_dither_stable = 0;

/* ---- CGB gray conversion (CrankBoy's defaults) ---- */
uint8_t cgb_blend_stage = 0;
uint8_t cgb_gray_lum_min = 0;
uint8_t cgb_gray_lum_max = 93;
int8_t cgb_gray_bias = 0;
bool cgb_hist_active = false;
bool cgb_contrast_active = false;
uint16_t cgb_thresh[3] = {3, 2, 1};
uint16_t cgb_thresh_delta = 1;

/* ---- Code relocation (Playdate ITCM): everything runs in place ---- */
intptr_t pgb_draw_reloc_offset = 0;
intptr_t pgb_rare_reloc_offset = 0;
intptr_t pgb_hle_reloc_offset = 0;
intptr_t pgb_apu_write_reloc_offset = 0;
intptr_t pgb_apu_sample_gen_reloc_offset = 0;

/* ---- Changed-line tracking: set by the frontend (GbEmulator) ---- */
uint8_t* pgb_dirty_prev = NULL;
uint16_t* pgb_dirty_flags = NULL;
uint8_t pgb_dirty_skip = 0;

/* ---- Playdate screen placement: only read by update_fb_dirty_lines(), unused here ---- */
int game_picture_x_offset = 0;
unsigned game_picture_scaling = 0;
int game_picture_y_top = 0;
int game_picture_y_bottom = LCD_HEIGHT;

/* ---- Accurate-sound ring (unused with sound mode 1, but referenced) ---- */
#ifdef ARDUINO
EXT_RAM_BSS_ATTR
#endif
AudioSyncBuffer g_audio_sync_buffer;
volatile int g_audio_resync_requested = 0;

/* ---- Simulator-build hooks (crankboy_shim.h builds the core with TARGET_SIMULATOR) ---- */
pthread_mutex_t audio_mutex = PTHREAD_MUTEX_INITIALIZER;
volatile int g_trace_frames_remaining = 0;

void __gb_on_breakpoint(gb_s* gb, int breakpoint_number) {
  (void)gb;
  (void)breakpoint_number;
}

/* ---- Playdate API stand-in ---- */

static void cb_log(const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
#ifdef ARDUINO
  char line[160];
  vsnprintf(line, sizeof(line), fmt, args);
  printf("[GB] %s\n", line);
#else
  vfprintf(stderr, fmt, args);
  fputc('\n', stderr);
#endif
  va_end(args);
}

static void cb_set_peripherals(PDPeripherals mask) { (void)mask; }

static void cb_accelerometer(float* x, float* y, float* z) {
  if (x) *x = 0.0f;
  if (y) *y = 0.0f;
  if (z) *z = 0.0f;
}

static void cb_mark_rows(int start, int end) {
  (void)start;
  (void)end;
}

static const struct cb_playdate_sys cb_sys = {cb_log, cb_set_peripherals, cb_accelerometer};
static const struct cb_playdate_gfx cb_gfx = {cb_mark_rows};
static PlaydateAPI cb_api = {&cb_sys, &cb_gfx};
PlaydateAPI* playdate = &cb_api;

/* ---- Allocation: PSRAM on the device (ROM banks, cart RAM, palettes) ---- */

void* cb_malloc(size_t size) {
#ifdef ARDUINO
  return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
  return malloc(size);
#endif
}

void* cb_realloc(void* ptr, size_t size) {
#ifdef ARDUINO
  return heap_caps_realloc(ptr, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
  return realloc(ptr, size);
#endif
}

void cb_free(void* ptr) { free(ptr); }

void* mallocz(size_t size) {
  void* p = cb_malloc(size);
  if (p) memset(p, 0, size);
  return p;
}

char* aprintf(const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  va_list copy;
  va_copy(copy, args);
  const int n = vsnprintf(NULL, 0, fmt, copy);
  va_end(copy);
  char* out = n >= 0 ? (char*)cb_malloc((size_t)n + 1) : NULL;
  if (out) vsnprintf(out, (size_t)n + 1, fmt, args);
  va_end(args);
  return out;
}

#endif
