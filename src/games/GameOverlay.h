#pragma once

/**
 * @file GameOverlay.h
 * @brief Optional artwork drawn over the game-mode screen (everything but the game itself).
 *
 * Overlays are 960 x 540 PNGs in /Games/Overlays (OverlayPng.h describes how they are drawn).
 * They only change how the screen looks: touch areas stay where they are. The chosen one is
 * named in /Games/.overlay. Template.png, written there by ensureTemplate(), marks every
 * button and the game screen with guides that never show on the device; Example.png next to
 * it is a finished overlay built on the same layout.
 *
 * The selection side runs in the normal UI (GamesActivity); load() and apply() run in game mode.
 */

#if INX_HAS_GAMES

#include <FastEPD.h>

#include <string>
#include <vector>

namespace GameOverlay {

constexpr const char* DIRECTORY = "/Games/Overlays";
constexpr const char* TEMPLATE_NAME = "Template.png";
constexpr const char* EXAMPLE_NAME = "Example.png";

/** File name of the chosen overlay, or "" for none. */
std::string selected();
void select(const std::string& name);
/** PNG files in DIRECTORY, sorted. */
std::vector<std::string> list();
/** Creates DIRECTORY and writes Template.png and Example.png into it where missing. */
void ensureTemplate();

/** Game mode: loads the chosen overlay. Returns nullptr on success or with none chosen, else why it failed. */
const char* load();
/** Game mode: draws the overlay into the rectangle (panel pixels), leaving the game area alone. */
void apply(FASTEPD& epd, int x, int y, int w, int h);

}  // namespace GameOverlay

#endif  // INX_HAS_GAMES
