#pragma once

/**
 * @file GameMode.h
 * @brief Games run in a separate boot of the firmware that owns the panel directly.
 *
 * freeink-sdk keeps its LovyanGFX panel driver to itself, and it cannot hand the bus over and
 * take it back. So launching a game records what to run in RTC memory (which survives a software
 * restart) and restarts; setup() sees the record before it starts the SDK display and calls
 * run(), which drives the panel through GameDisplay (FastEPD) instead. Leaving a game restarts
 * again into the normal UI. The record is cleared as soon as run() starts, so a crash in a game
 * comes back to the reader rather than looping.
 */

#if INX_HAS_GAMES

#include <string>

namespace GameMode {

constexpr const char* ROMS_DIRECTORY = "/Games";

/** A game launch is waiting for this boot. */
bool pending();

/** Restarts into a game: a .gb / .gbc ROM path on the SD card. */
[[noreturn]] void launchGame(const char* romPath);

/** Runs the pending launch. Call from setup() after gpio and SD, before the SDK display. */
[[noreturn]] void run();

/** Restarts back into the normal UI. */
[[noreturn]] void exitToReader();

}  // namespace GameMode

#endif  // INX_HAS_GAMES
