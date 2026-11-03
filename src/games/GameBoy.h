#pragma once

/**
 * @file GameBoy.h
 * @brief Game mode screen that plays a Game Boy / Game Boy Color ROM.
 *
 * Loads the ROM into PSRAM, runs the CrankBoy core (gb/GbSession.h) at the Game Boy's frame
 * rate, draws each frame 3x scaled and dithered to four shades into the game area, and lets
 * the continuous scan (GameVideo.h) put it on the panel. Touch controls sit either side of the
 * game area. Battery saves are kept next to the ROM as <name>.sav.
 */

#if INX_HAS_GAMES

namespace GameBoy {

/** Plays `romPath` until EXIT or the power button. Returns only if the ROM could not be started. */
void run(const char* romPath);

}  // namespace GameBoy

#endif  // INX_HAS_GAMES
