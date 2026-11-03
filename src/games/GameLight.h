#pragma once

/**
 * @file GameLight.h
 * @brief Frontlight in game mode: the reader's saved setting on entry, in-game changes saved on exit.
 *
 * Normal boots apply the saved frontlight in BootActivity, which game mode never reaches. Steps use
 * frontlight_ui::stepBrightness() so the T5's unlit 8-bit range below INX_FRONTLIGHT_MIN_PERCENT
 * is skipped, as in the reader.
 */

#if INX_HAS_GAMES

#include <cstdint>

namespace GameLight {

/** Applies the saved setting. Call once at the start of game mode (after SD). */
void restore();
void toggle();
void step(bool up);
bool isOn();
uint8_t percent();
/** Writes the setting back if it changed in game. Call before leaving game mode. */
void save();

}  // namespace GameLight

#endif  // INX_HAS_GAMES
