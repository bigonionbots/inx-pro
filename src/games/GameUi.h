#pragma once

/**
 * @file GameUi.h
 * @brief Screen layout, touch input and the top bar shared by the game-mode screens.
 *
 * Layout (panel pixels, 960 x 540 landscape): a 54-pixel top bar (title, LIGHT, -, +, EXIT),
 * the game area (a Game Boy screen at 3x: 480 x 432) centred under it, a bottom bar below the
 * game area, and the side columns either side of it for the on-screen controls.
 */

#if INX_HAS_GAMES

#include <FastEPD.h>

#include <cstdint>

namespace GameUi {

constexpr int BAR_H = 54;
constexpr int AREA_X = 240;
constexpr int AREA_Y = BAR_H;
constexpr int AREA_W = 480;
constexpr int AREA_H = 432;
constexpr int BOTTOM_Y = AREA_Y + AREA_H;  // bottom bar: 486..539
constexpr int MAX_CONTACTS = 5;

/** Top-bar buttons (x and width; all are BAR_H tall at y 0). The status text line is left of them. */
constexpr int LIGHT_X = 480;
constexpr int LIGHT_W = 120;
constexpr int DIM_X = 610;
constexpr int BRIGHT_X = 725;
constexpr int STEP_W = 105;
constexpr int EXIT_X = 840;
constexpr int EXIT_W = 120;

/**
 * Game Boy on-screen controls. scripts/make_game_overlay_template.py draws the same layout into
 * the overlay template: keep the two in step.
 */
namespace GameBoyControls {
constexpr int DPAD_X = AREA_X / 2;
constexpr int DPAD_Y = 300;
constexpr int DPAD_ARM = 110;   // centre to tip
constexpr int DPAD_WIDTH = 70;  // arm thickness
constexpr int DPAD_DEAD = 18;
constexpr int A_X = 895;
constexpr int A_Y = 250;
constexpr int B_X = 785;
constexpr int B_Y = 340;
constexpr int AB_R = 52;
constexpr int AB_REACH = 115;  // touches this close to a button count as it
constexpr int SELECT_X = 300;
constexpr int START_X = 520;
constexpr int SMALL_W = 140;  // SELECT / START, from BOTTOM_Y + 6 to the bottom edge - 4
}  // namespace GameBoyControls

struct Contact {
  int x;
  int y;
};

/** Current touch contacts in panel pixels; returns how many. Call after gpio.update(). */
int contacts(Contact* out, int max);

/** FONT_12x16 text, black on white. */
void text(FASTEPD& epd, int x, int y, const char* s);
/** Width of `s` in FONT_12x16. */
int textWidth(const char* s);

enum class BarButton : uint8_t { None, Light, Dimmer, Brighter, Exit };

/** Draws the top bar: two short title lines on the left, the light controls and EXIT on the right. */
void drawBar(FASTEPD& epd, const char* line1, const char* line2);

/**
 * Top-bar touch handling. Each button acts once per press. LIGHT / - / + are applied here (and
 * the LIGHT readout redrawn into the buffer); the caller pushes rows 0..BAR_H-1 to the panel
 * when update() returns one of them, and leaves game mode on Exit.
 */
class BarInput {
 public:
  /** @return the button newly pressed this frame, or None. */
  BarButton update(FASTEPD& epd, const Contact* touches, int count);

 private:
  BarButton held = BarButton::None;
};

}  // namespace GameUi

#endif  // INX_HAS_GAMES
