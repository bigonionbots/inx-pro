#pragma once

/**
 * @file GameVideo.h
 * @brief Continuous panel scan for game mode: what is drawn appears within a few scans.
 *
 * Instead of one update per frame (several full scans plus a clean pass), a task on core 0
 * scans the panel continuously. Each scan picks up whatever changed in FastEPD's current
 * buffer and drives each changed pixel for a fixed number of scans (GameVideoRow.h), so
 * drawing and refreshing overlap.
 *
 * Draw into GameDisplay::epd() as usual (FastEPD 1-bpp calls), then call markRows() for the
 * rows that changed; rows that are neither marked nor still settling are skipped quickly.
 * While the scan runs, do not call FastEPD's own update functions.
 */

#if INX_HAS_GAMES

#include <cstdint>

namespace GameVideo {

/** Default scans per colour change. FastEPD's partial update uses 4. */
constexpr int DEFAULT_PASSES = 4;

/** Starts scanning (panel powered by GameDisplay::begin()). Assumes the panel shows the current buffer. */
bool start(int passes = DEFAULT_PASSES);
/** Stops after the current scan and leaves every pixel undriven. */
void stop();
/** Queues rows top..bottom (inclusive, panel rows) for the next scan. */
void markRows(int top, int bottom);
/** Full clearing refresh of the whole panel to remove ghosting; pauses the scan meanwhile. */
void clean();
/** Completed scans since start(), for timing. */
uint32_t scans();

}  // namespace GameVideo

#endif  // INX_HAS_GAMES
