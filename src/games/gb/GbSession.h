#pragma once

/**
 * @file GbSession.h
 * @brief Plain C interface to one running Game Boy / Color game (the CrankBoy core).
 *
 * The core's headers are C with GNU extensions, so the C++ side of game mode sees only this.
 * One session at a time.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Game Boy screen. gbs_lcd() is GBS_LCD_H rows of GBS_LCD_ROW_BYTES bytes; pixel x of a row is
 *  bits (x % 4) * 2 of byte x / 4, a shade from 0 (white) to 3 (black). */
#define GBS_LCD_W 160
#define GBS_LCD_H 144
#define GBS_LCD_ROW_BYTES 40

/** Joypad bits for gbs_set_buttons(): set = pressed. */
enum {
  GBS_A = 1 << 0,
  GBS_B = 1 << 1,
  GBS_SELECT = 1 << 2,
  GBS_START = 1 << 3,
  GBS_RIGHT = 1 << 4,
  GBS_LEFT = 1 << 5,
  GBS_UP = 1 << 6,
  GBS_DOWN = 1 << 7,
};

/**
 * Starts a game. `rom` must stay valid while the session runs. Runs in Color mode when the
 * cartridge supports it. Returns false (and logs why) if the core rejects the ROM.
 */
bool gbs_open(uint8_t* rom, size_t rom_size);
void gbs_close(void);
bool gbs_is_color(void);
/** Cartridge RAM (battery saves). Size 0 when the cartridge has none. */
uint8_t* gbs_cart_ram(size_t* size);
bool gbs_has_battery(void);
/** True once after any write to cartridge RAM since the last call. */
bool gbs_take_cart_ram_written(void);
void gbs_set_buttons(uint8_t pressed);
void gbs_run_frame(void);
const uint8_t* gbs_lcd(void);

#ifdef __cplusplus
}
#endif
