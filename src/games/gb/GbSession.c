/**
 * @file GbSession.c
 * @brief Definitions for the C session wrapper around the CrankBoy core.
 *
 * This file also compiles the core itself (PGB_IMPL): most of its functions are declared only
 * inside the implementation, so the code that calls them has to be in the same translation unit.
 */

#if INX_HAS_GAMES || defined(GB_HOST_TEST)

#include "GbSession.h"

#include <stdlib.h>
#include <string.h>

#define PGB_IMPL
#include "../../../third_party/crankboy/libs/peanut_gb.h"

#ifdef ARDUINO
#include <esp_heap_caps.h>
#define GBS_FAST_ALLOC(n) heap_caps_calloc(1, (n), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
#else
#define GBS_FAST_ALLOC(n) calloc(1, (n))
#endif

static gb_s* gb;
static uint8_t* wram;
static uint8_t* vram;
static uint8_t lcd[LCD_BUFFER_BYTES] __attribute__((aligned(32)));

static void on_error(gb_s* g, const enum gb_error_e err, const uint16_t addr) {
  (void)g;
  playdate->system->logToConsole("core error %d at %04x", (int)err, addr);
}

static bool sound_on;
static CB_GameContext sound_context;
static CB_GameScene sound_scene;
static CB_GameScene* sound_scene_ref;  // audio_callback() takes the scene through a pointer

bool gbs_open(uint8_t* rom, const size_t rom_size, const bool sound) {
  gbs_close();
  gb = GBS_FAST_ALLOC(sizeof(gb_s));
  wram = GBS_FAST_ALLOC(WRAM_SIZE_CGB);
  vram = GBS_FAST_ALLOC(VRAM_SIZE_CGB);
  if (!gb || !wram || !vram) {
    playdate->system->logToConsole("out of memory");
    gbs_close();
    return false;
  }
  const bool color = (gb_get_models_supported(rom) & GB_SUPPORT_CGB) != 0;
  const enum gb_init_error_e ret = gb_init(gb, wram, vram, lcd, rom, rom_size, on_error, NULL, color);
  if (ret != GB_INIT_NO_ERROR && ret != GB_INIT_NO_ERROR_BUT_REQUIRES_CGB) {
    playdate->system->logToConsole("ROM rejected (error %d)", (int)ret);
    gbs_close();
    return false;
  }
  gb_reset(gb, color);
  const size_t ram_size = gb_get_save_size(gb);
  gb->gb_cart_ram = ram_size ? cb_malloc(ram_size) : NULL;
  if (ram_size && !gb->gb_cart_ram) {
    playdate->system->logToConsole("out of memory for cartridge RAM");
    gbs_close();
    return false;
  }
  if (gb->gb_cart_ram) memset(gb->gb_cart_ram, 0xFF, ram_size);
  gb->gb_cart_ram_size = (uint32_t)ram_size;
  gb_init_lcd(gb);
  // gb_init() turned sound on, so gb_reset() has already initialised the sound hardware.
  gb->direct.sound = sound ? 1 : 0;
  sound_on = sound;
  if (sound) {
    audio_enabled = 1;
    audio_muted = 0;
    // Full rate. The half-rate setting (1) renders every other sample and interpolates, but on
    // this (non-Playdate) build path it comes out as noise: 4-5x the zero crossings of full rate.
    preferences_sample_rate = 0;
    sound_context.gb = gb;
    sound_scene.context = &sound_context;
    sound_scene.audioLocked = false;
    sound_scene.is_stereo = false;  // both channels mixed into one
    sound_scene_ref = &sound_scene;
  }
  gb->direct.joypad = 0xFF;
  return true;
}

void gbs_close(void) {
  sound_on = false;
  sound_scene_ref = NULL;
  if (gb && gb->gb_cart_ram) cb_free(gb->gb_cart_ram);
  free(gb);
  free(wram);
  free(vram);
  gb = NULL;
  wram = NULL;
  vram = NULL;
}

bool gbs_is_color(void) { return gb && gb->is_cgb_mode; }

uint8_t* gbs_cart_ram(size_t* size) {
  *size = gb ? gb->gb_cart_ram_size : 0;
  return gb ? gb->gb_cart_ram : NULL;
}

bool gbs_has_battery(void) { return gb && gb->cart_battery; }

bool gbs_take_cart_ram_written(void) {
  if (!gb || !gb->direct.sram_updated) return false;
  gb->direct.sram_updated = 0;
  return true;
}

void gbs_set_buttons(const uint8_t pressed) {
  if (gb) gb->direct.joypad = (uint8_t)~pressed;  // the core's joypad bits are active low
}

void gbs_run_frame(void) {
  if (!gb) return;
  if (gb->is_cgb_mode) {
    gb_run_frame__cgb(gb);
  } else {
    gb_run_frame__dmg(gb);
  }
}

const uint8_t* gbs_lcd(void) { return lcd; }

/* audio_callback() renders in chunks of up to 256 samples, and in mono (left == right) it
 * advances only the left pointer between them: from the second chunk on it takes its stereo
 * path and adds the right channel's share back into the first chunk, which doubled some samples
 * and left gaps, heard as crackle on low notes. The Playdate asks for at most 256 samples at a
 * time, so it never sees this; asking for no more here keeps the mono path within one chunk. */
#define GBS_AUDIO_CALL_MAX 256

void gbs_render_audio(int16_t* out, const int samples) {
  for (int done = 0; done < samples;) {
    const int n = samples - done < GBS_AUDIO_CALL_MAX ? samples - done : GBS_AUDIO_CALL_MAX;
    if (!gb || !sound_on || !audio_callback(&sound_scene_ref, out + done, out + done, n)) {
      memset(out + done, 0, (size_t)n * sizeof(int16_t));
    }
    done += n;
  }
}

#endif
