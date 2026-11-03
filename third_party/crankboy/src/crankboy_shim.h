/**
 * @file crankboy_shim.h
 * @brief Stand-ins for the CrankBoy (Playdate) app headers that the vendored core includes.
 *
 * The core in ../libs is CrankBoy's Peanut-GB fork, copied unmodified. It includes
 * "../src/app.h", "../src/utility.h", "../src/preferences.h", "../src/dtcm.h" and
 * "../src/scenes/game_scene.h" from the Playdate app; those names in this directory all
 * include this file instead. It supplies the macros, preference globals and a minimal
 * `playdate` API object the core touches, and nothing else.
 */
#ifndef CRANKBOY_SHIM_H
#define CRANKBOY_SHIM_H

/* Build the core the way CrankBoy builds it for its desktop simulator. On the Playdate target
 * the core puts its hot code in custom sections (.dtcm.draw.*, .rare.*, .hle.*, .apu_write)
 * that CrankBoy relocates into tightly-coupled memory at run time. The ESP-IDF linker script
 * has no rule for them, so they would land past the flash text the MMU maps for code. */
#ifndef TARGET_SIMULATOR
#define TARGET_SIMULATOR 1
#endif

#include <pthread.h>

#include <stdarg.h>
#ifndef __cplusplus
#include <stdatomic.h>
#endif
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Attributes and helpers (utility.h / app.h) ---- */

#define FORCE_INLINE __attribute__((always_inline)) inline
#define __section__(x)
#define __space
#define __shell __attribute__((noinline))
#ifndef likely
#define likely(x) (__builtin_expect(!!(x), 1))
#endif
#ifndef unlikely
#define unlikely(x) (__builtin_expect(!!(x), 0))
#endif
#define clalign __attribute__((aligned(32)))
#define CB_ASSERT(x) ((void)0)
#define CPU_VALIDATE 0
#define CB_DEBUG false
#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif
#define CB_MIN(x, y) MIN(x, y)
#define CB_MAX(x, y) MAX(x, y)
#define DTCM_VERIFY() 1
#define DTCM_VERIFY_DEBUG() 1

/* Playdate screen geometry, used only by update_fb_dirty_lines(), which this port never calls. */
#define CB_LCD_WIDTH 320
#define CB_LCD_HEIGHT 240
#define CB_LCD_X 40
#define CB_LCD_Y 0
#define PLAYDATE_ROW_STRIDE 52
/* Crank register scale (CrankBoy's crank extension; the T5 has no crank). */
#define CRANK_MENU_DELTA_BINANGLE 0x2800
extern int game_picture_x_offset;
extern unsigned game_picture_scaling;
extern int game_picture_y_top;
extern int game_picture_y_bottom;

enum cgb_support_e {
  GB_SUPPORT_DMG = 1,
  GB_SUPPORT_CGB = 2,
  GB_SUPPORT_DMG_AND_CGB = 3,
};

static FORCE_INLINE uint8_t reverse_bits_u8(uint8_t b) {
  b = (uint8_t)(((b & 0xF0u) >> 4) | ((b & 0x0Fu) << 4));
  b = (uint8_t)(((b & 0xCCu) >> 2) | ((b & 0x33u) << 2));
  b = (uint8_t)(((b & 0xAAu) >> 1) | ((b & 0x55u) << 1));
  return b;
}

/* ---- Preferences (preferences.h): fixed values, set by the port at start-up ---- */

extern int preferences_sound_mode;   /* 0 off, 1 fast, 2 accurate */
extern int preferences_sample_rate;  /* 0 full rate */
extern int preferences_high_pass_filter;
extern int preferences_uncap_fps;
extern int preferences_cgb_speed;
extern int preferences_cgb_gamma;
extern int preferences_cgb_bias_auto;
extern int preferences_dither_line;
extern int preferences_dither_stable;

/* ---- The parts of the Playdate API the core calls ---- */

typedef enum { kNone = 0, kAccelerometer = 1 } PDPeripherals;

struct cb_playdate_sys {
  void (*logToConsole)(const char* fmt, ...);
  void (*setPeripheralsEnabled)(PDPeripherals mask);
  void (*getAccelerometer)(float* x, float* y, float* z);
};
struct cb_playdate_gfx {
  void (*markUpdatedRows)(int start, int end);
};
typedef struct {
  const struct cb_playdate_sys* system;
  const struct cb_playdate_gfx* graphics;
} PlaydateAPI;

extern PlaydateAPI* playdate;

/* Simulator-build global: the audio callback's lock. Defined by the port. */
extern pthread_mutex_t audio_mutex;

/* ---- Allocation and strings (utility.h); defined by the port ---- */

void* cb_malloc(size_t size);
void* cb_realloc(void* ptr, size_t size);
void cb_free(void* ptr);
void* mallocz(size_t size);
/** printf into a new cb_malloc() buffer. */
char* aprintf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

static FORCE_INLINE uint32_t reverse_bits_in_each_byte_conditional_u16(uint16_t b, bool condition) {
  if (!condition) return b;
  return (uint32_t)(reverse_bits_u8(b & 0xFF) | (reverse_bits_u8((b >> 8) & 0xFF) << 8));
}

/* ---- Accurate-mode audio ring (app.h). Only used with preferences_sound_mode == 2. ---- */

#ifndef __cplusplus
#define AUDIO_RING_BUFFER_SIZE 8192
#define AUDIO_RING_BUFFER_MASK (AUDIO_RING_BUFFER_SIZE - 1)
typedef struct {
  int16_t left[AUDIO_RING_BUFFER_SIZE];
  int16_t right[AUDIO_RING_BUFFER_SIZE];
  atomic_uint write_pos;
  atomic_uint read_pos;
} AudioSyncBuffer;
extern AudioSyncBuffer g_audio_sync_buffer;
extern volatile int g_audio_resync_requested;
#endif

/* ---- The game-scene fields minigb_apu's audio_callback() reads (scenes/game_scene.h) ---- */

#include "../libs/pgb/pgb_version.h"
#define CB_SHIM_GB_TAG__(v) gb_s_v##v
#define CB_SHIM_GB_TAG_(v) CB_SHIM_GB_TAG__(v)
struct CB_SHIM_GB_TAG_(PGB_VERSION);

typedef struct {
  struct CB_SHIM_GB_TAG_(PGB_VERSION) * gb;
} CB_GameContext;

typedef struct {
  CB_GameContext* context;
  bool audioLocked;
  bool is_stereo;
} CB_GameScene;

#ifdef __cplusplus
}
#endif

#endif /* CRANKBOY_SHIM_H */
