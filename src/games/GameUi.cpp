/**
 * @file GameUi.cpp
 * @brief Definitions for the shared game-mode layout, touch input and top bar.
 */

#include "GameUi.h"

#if INX_HAS_GAMES

#include <BoardConfig.h>
#include <HalGPIO.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "GameDisplay.h"
#include "GameLight.h"

extern HalGPIO gpio;

namespace GameUi {

namespace {

constexpr int CHAR_W = 12;  // FONT_12x16

BarButton barButtonAt(const Contact& c) {
  if (c.y >= BAR_H) return BarButton::None;
  if (c.x >= EXIT_X) return BarButton::Exit;
  if (c.x >= BRIGHT_X && c.x < BRIGHT_X + STEP_W) return BarButton::Brighter;
  if (c.x >= DIM_X && c.x < DIM_X + STEP_W) return BarButton::Dimmer;
  if (c.x >= LIGHT_X && c.x < LIGHT_X + LIGHT_W) return BarButton::Light;
  return BarButton::None;
}

/** Redraws the LIGHT button's readout (OFF or the percentage). */
void drawLightButton(FASTEPD& epd) {
  epd.fillRect(LIGHT_X + 1, 1, LIGHT_W - 2, BAR_H - 2, BBEP_WHITE);
  text(epd, LIGHT_X + (LIGHT_W - textWidth("LIGHT")) / 2, 6, "LIGHT");
  char value[8];
  if (GameLight::isOn()) {
    std::snprintf(value, sizeof(value), "%u%%", static_cast<unsigned>(GameLight::percent()));
  } else {
    std::snprintf(value, sizeof(value), "OFF");
  }
  text(epd, LIGHT_X + (LIGHT_W - textWidth(value)) / 2, 30, value);
}

void boxedLabel(FASTEPD& epd, const int x, const int w, const char* label) {
  epd.drawRect(x, 0, w, BAR_H, BBEP_BLACK);
  text(epd, x + (w - textWidth(label)) / 2, 18, label);
}

}  // namespace

int contacts(Contact* out, const int max) {
  const InputManager::TouchSnapshot snapshot = gpio.touchSnapshot();
  const auto& t = BoardConfig::ACTIVE.touch;
  const float w = t.rawMaxX > t.rawMinX ? static_cast<float>(t.rawMaxX - t.rawMinX) : 1.0f;
  const float h = t.rawMaxY > t.rawMinY ? static_cast<float>(t.rawMaxY - t.rawMinY) : 1.0f;
  int n = 0;
  for (int i = 0; i < snapshot.count && n < max; ++i) {
    if (!snapshot.points[i].point.valid) continue;
    out[n].x = std::min(GameDisplay::WIDTH - 1, static_cast<int>(snapshot.points[i].point.x / w * GameDisplay::WIDTH));
    out[n].y = std::min(GameDisplay::HEIGHT - 1, static_cast<int>(snapshot.points[i].point.y / h * GameDisplay::HEIGHT));
    ++n;
  }
  return n;
}

void text(FASTEPD& epd, const int x, const int y, const char* s) {
  epd.setTextColor(BBEP_BLACK, BBEP_WHITE);
  epd.drawString(s, x, y);
}

int textWidth(const char* s) { return CHAR_W * static_cast<int>(std::strlen(s)); }

void drawBar(FASTEPD& epd, const char* line1, const char* line2) {
  epd.setFont(FONT_12x16);
  epd.fillRect(0, 0, GameDisplay::WIDTH, BAR_H, BBEP_WHITE);
  if (line1) text(epd, 8, 8, line1);
  if (line2) text(epd, 8, 30, line2);
  epd.drawRect(LIGHT_X, 0, LIGHT_W, BAR_H, BBEP_BLACK);
  drawLightButton(epd);
  boxedLabel(epd, DIM_X, STEP_W, "-");
  boxedLabel(epd, BRIGHT_X, STEP_W, "+");
  boxedLabel(epd, EXIT_X, EXIT_W, "EXIT");
}

BarButton BarInput::update(FASTEPD& epd, const Contact* touches, const int count) {
  BarButton pressed = BarButton::None;
  for (int i = 0; i < count; ++i) {
    const BarButton button = barButtonAt(touches[i]);
    if (button != BarButton::None) pressed = button;
  }
  const bool fresh = pressed != held && pressed != BarButton::None;
  held = pressed;
  if (!fresh) return BarButton::None;
  if (pressed == BarButton::Light) GameLight::toggle();
  if (pressed == BarButton::Dimmer) GameLight::step(false);
  if (pressed == BarButton::Brighter) GameLight::step(true);
  if (pressed != BarButton::Exit) drawLightButton(epd);
  return pressed;
}

}  // namespace GameUi

#endif  // INX_HAS_GAMES
