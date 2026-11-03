#pragma once

/**
 * @file GameDisplay.h
 * @brief Direct panel access for game mode on the LilyGo T5 S3 Pro Lite (H752-01), via FastEPD.
 *
 * inx-pro's UI drives the panel through freeink-sdk (LovyanGFX), which refreshes the whole panel
 * per frame. Games need fast partial updates, so game mode (GameMode.h) boots without the SDK
 * display and hands the panel to FastEPD (third_party/FastEPD) instead. The H752-01 has no
 * FastEPD profile (its PMIC and output enable sit behind a PCA9535), so this registers a custom
 * panel: FastEPD's parallel bus on the SDK's data and timing pins, row control on the gate GPIOs,
 * and power through freeink-sdk's own LgfxEpdConfig power hooks.
 *
 * Coordinates are the panel's native landscape frame, 960 x 540.
 */

#if INX_HAS_GAMES

#include <FastEPD.h>

namespace GameDisplay {

constexpr int WIDTH = 960;
constexpr int HEIGHT = 540;

/** Brings the panel up and clears it. False if the panel did not power up. */
bool begin();
FASTEPD& epd();
/** Powers the panel rails down (before restarting out of game mode). */
void end();

}  // namespace GameDisplay

#endif  // INX_HAS_GAMES
