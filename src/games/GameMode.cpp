/**
 * @file GameMode.cpp
 * @brief Definitions for the game-mode boot path.
 */

#include "GameMode.h"

#if INX_HAS_GAMES

#include <Arduino.h>
#include <BoardConfig.h>
#include <HalGPIO.h>
#include <esp_attr.h>
#include <esp_system.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "GameDisplay.h"
#include "GameLight.h"

extern HalGPIO gpio;

namespace GameMode {

namespace {

constexpr uint32_t LAUNCH_MAGIC = 0x47414D45;  // "GAME"

enum class Kind : uint8_t { DisplayTest = 1 };

struct LaunchRecord {
  uint32_t magic;
  Kind kind;
  char path[200];
};

RTC_NOINIT_ATTR LaunchRecord launchRecord;

[[noreturn]] void restartWith(const Kind kind, const char* path) {
  launchRecord.kind = kind;
  std::strncpy(launchRecord.path, path ? path : "", sizeof(launchRecord.path) - 1);
  launchRecord.path[sizeof(launchRecord.path) - 1] = '\0';
  launchRecord.magic = LAUNCH_MAGIC;
  esp_restart();
  for (;;) {
  }
}

// ---------------------------------------------------------------------------------------------
// Touch, in panel pixels.

struct Contact {
  int x;
  int y;
};

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

// ---------------------------------------------------------------------------------------------
// Milestone 1: panel and touch test.

constexpr int AREA_X = 240;  // a Game Boy screen at 3x: 480 x 432, centred
constexpr int AREA_Y = 54;
constexpr int AREA_W = 480;
constexpr int AREA_H = 432;
constexpr int BLOCK = 48;
constexpr int EXIT_X = 840;
constexpr int EXIT_W = 120;
constexpr int EXIT_H = 54;
// Frontlight controls in the top bar, left of EXIT.
constexpr int LIGHT_X = 480;
constexpr int LIGHT_W = 120;
constexpr int DIM_X = 610;
constexpr int BRIGHT_X = 725;
constexpr int STEP_W = 105;
constexpr uint32_t PHASE_MS = 8000;

void text(FASTEPD& epd, const int x, const int y, const char* s) {
  epd.setTextColor(BBEP_BLACK, BBEP_WHITE);
  epd.drawString(s, x, y);
}

/** 2x2 Bayer dither of a Game Boy shade (0 white .. 3 black), the way game frames will be drawn. */
void ditherRect(FASTEPD& epd, const int x0, const int y0, const int w, const int h, const int shade) {
  constexpr int BAYER[2][2] = {{0, 2}, {3, 1}};
  constexpr int INK[4] = {0, 1, 3, 4};  // black pixels out of 4 per shade
  for (int y = y0; y < y0 + h; ++y) {
    for (int x = x0; x < x0 + w; ++x) {
      epd.drawPixelFast(x, y, BAYER[y & 1][x & 1] < INK[shade] ? BBEP_BLACK : BBEP_WHITE);
    }
  }
}

enum class BarButton : uint8_t { None, Light, Dimmer, Brighter, Exit };

BarButton barButtonAt(const Contact& c) {
  if (c.y >= EXIT_H) return BarButton::None;
  if (c.x >= EXIT_X) return BarButton::Exit;
  if (c.x >= BRIGHT_X && c.x < BRIGHT_X + STEP_W) return BarButton::Brighter;
  if (c.x >= DIM_X && c.x < DIM_X + STEP_W) return BarButton::Dimmer;
  if (c.x >= LIGHT_X && c.x < LIGHT_X + LIGHT_W) return BarButton::Light;
  return BarButton::None;
}

/** Redraws the LIGHT button's readout (OFF or the percentage). The caller pushes the top bar. */
void drawLightButton(FASTEPD& epd) {
  epd.fillRect(LIGHT_X + 1, 1, LIGHT_W - 2, EXIT_H - 2, BBEP_WHITE);
  text(epd, LIGHT_X + 30, 6, "LIGHT");
  char value[8];
  if (GameLight::isOn()) {
    std::snprintf(value, sizeof(value), "%u%%", static_cast<unsigned>(GameLight::percent()));
  } else {
    std::snprintf(value, sizeof(value), "OFF");
  }
  text(epd, LIGHT_X + (LIGHT_W - 12 * static_cast<int>(std::strlen(value))) / 2, 30, value);
}

[[noreturn]] void runDisplayTest() {
  FASTEPD& epd = GameDisplay::epd();
  epd.setFont(FONT_12x16);
  epd.fillScreen(BBEP_WHITE);
  text(epd, 8, 8, "Game display test");
  text(epd, 8, 30, "Top-left corner. Fingers are marked.");
  epd.drawRect(LIGHT_X, 0, LIGHT_W, EXIT_H, BBEP_BLACK);
  drawLightButton(epd);
  epd.drawRect(DIM_X, 0, STEP_W, EXIT_H, BBEP_BLACK);
  text(epd, DIM_X + 46, 18, "-");
  epd.drawRect(BRIGHT_X, 0, STEP_W, EXIT_H, BBEP_BLACK);
  text(epd, BRIGHT_X + 46, 18, "+");
  epd.drawRect(EXIT_X, 0, EXIT_W, EXIT_H, BBEP_BLACK);
  text(epd, EXIT_X + 30, 18, "EXIT");
  epd.drawRect(AREA_X - 1, AREA_Y - 1, AREA_W + 2, AREA_H + 2, BBEP_BLACK);
  for (int shade = 0; shade < 4; ++shade) ditherRect(epd, AREA_X + shade * 120, AREA_Y, 120, 40, shade);
  epd.fullUpdate(CLEAR_FAST, true);

  int bx = AREA_X + 10, by = AREA_Y + 60, dx = 6, dy = 4;
  uint32_t phaseStart = millis();
  int phase = 0;  // 0: update only the rows the block touches, 1: the whole game area
  uint32_t updates = 0, updateUs = 0;
  char line[96];
  Contact marks[InputManager::MAX_TOUCH_CONTACTS] = {};
  int markCount = 0;
  BarButton held = BarButton::None;  // bar buttons act once per press, not every frame

  for (;;) {
    gpio.update();
    if (gpio.wasPressed(HalGPIO::BTN_POWER)) exitToReader();

    Contact touched[InputManager::MAX_TOUCH_CONTACTS];
    const int touchedCount = contacts(touched, InputManager::MAX_TOUCH_CONTACTS);
    BarButton pressed = BarButton::None;
    Contact now[InputManager::MAX_TOUCH_CONTACTS];
    int n = 0;
    for (int i = 0; i < touchedCount; ++i) {
      const BarButton button = barButtonAt(touched[i]);
      if (button != BarButton::None) {
        pressed = button;
      } else if (touched[i].y >= EXIT_H + 19) {
        now[n++] = touched[i];  // marks stay clear of the bar
      }
    }
    if (pressed != held && pressed != BarButton::None) {
      if (pressed == BarButton::Exit) exitToReader();
      if (pressed == BarButton::Light) GameLight::toggle();
      if (pressed == BarButton::Dimmer) GameLight::step(false);
      if (pressed == BarButton::Brighter) GameLight::step(true);
      drawLightButton(epd);
      epd.partialUpdate(true, 0, EXIT_H);
    }
    held = pressed;

    // Erase old finger marks, draw new ones (outside the game area they cost a partial update too).
    for (int i = 0; i < markCount; ++i) epd.fillCircle(marks[i].x, marks[i].y, 18, BBEP_WHITE);
    for (int i = 0; i < n; ++i) epd.drawCircle(now[i].x, now[i].y, 18, BBEP_BLACK);
    int touchTop = GameDisplay::HEIGHT, touchBottom = -1;
    for (int i = 0; i < markCount; ++i) {
      touchTop = std::min(touchTop, marks[i].y - 19);
      touchBottom = std::max(touchBottom, marks[i].y + 19);
    }
    for (int i = 0; i < n; ++i) {
      touchTop = std::min(touchTop, now[i].y - 19);
      touchBottom = std::max(touchBottom, now[i].y + 19);
    }
    std::copy(now, now + n, marks);
    markCount = n;

    // Move the block.
    const int oldY = by;
    epd.fillRect(bx, by, BLOCK, BLOCK, BBEP_WHITE);
    bx += dx;
    by += dy;
    if (bx < AREA_X || bx + BLOCK > AREA_X + AREA_W) {
      dx = -dx;
      bx += 2 * dx;
    }
    if (by < AREA_Y + 41 || by + BLOCK > AREA_Y + AREA_H) {
      dy = -dy;
      by += 2 * dy;
    }
    epd.fillRect(bx, by, BLOCK, BLOCK, BBEP_BLACK);

    int top = phase == 0 ? std::min(oldY, by) : AREA_Y;
    int bottom = phase == 0 ? std::max(oldY, by) + BLOCK : AREA_Y + AREA_H;
    if (touchBottom >= 0) {
      top = std::min(top, std::max(0, touchTop));
      bottom = std::max(bottom, std::min(GameDisplay::HEIGHT - 1, touchBottom));
    }
    const uint32_t started = micros();
    epd.partialUpdate(true, top, bottom);
    updateUs += micros() - started;
    ++updates;

    if (millis() - phaseStart >= PHASE_MS) {
      const uint32_t avgMs = updates ? updateUs / updates / 1000 : 0;
      std::snprintf(line, sizeof(line), "%s: %lu ms per update (%lu fps), %d fingers   ",
                    phase == 0 ? "Block rows only" : "Whole game area", static_cast<unsigned long>(avgMs),
                    static_cast<unsigned long>(avgMs ? 1000 / avgMs : 0), n);
      epd.fillRect(0, GameDisplay::HEIGHT - 44, GameDisplay::WIDTH, 44, BBEP_WHITE);
      text(epd, 8, GameDisplay::HEIGHT - 40 + (phase == 0 ? 0 : 20), line);
      INX_SERIAL.printf("[GAME] %s\n", line);
      epd.partialUpdate(true, GameDisplay::HEIGHT - 44, GameDisplay::HEIGHT - 1);
      phase ^= 1;
      phaseStart = millis();
      updates = 0;
      updateUs = 0;
    }
    vTaskDelay(1);  // let the idle task run (task watchdog)
  }
}

}  // namespace

bool pending() { return launchRecord.magic == LAUNCH_MAGIC; }

void launchDisplayTest() { restartWith(Kind::DisplayTest, nullptr); }

void run() {
  const LaunchRecord launch = launchRecord;
  launchRecord.magic = 0;  // a crash from here on boots back into the reader
  INX_SERIAL.printf("[GAME] game mode: kind=%u\n", static_cast<unsigned>(launch.kind));
  if (!GameDisplay::begin()) {
    INX_SERIAL.println("[GAME] panel did not power up");
    delay(1000);
    exitToReader();
  }
  GameLight::restore();
  switch (launch.kind) {
    case Kind::DisplayTest:
    default:
      runDisplayTest();
  }
}

void exitToReader() {
  GameLight::save();
  GameDisplay::end();
  launchRecord.magic = 0;
  esp_restart();
  for (;;) {
  }
}

}  // namespace GameMode

#endif  // INX_HAS_GAMES
