/**
 * @file GameMode.cpp
 * @brief Definitions for the game-mode boot path.
 */

#include "GameMode.h"

#if INX_HAS_GAMES

#include <Arduino.h>
#include <HalGPIO.h>
#include <esp_attr.h>
#include <esp_system.h>

#include <cstring>

#include "GameBoy.h"
#include "GameDisplay.h"
#include "GameLight.h"
#include "GameUi.h"

extern HalGPIO gpio;

namespace GameMode {

namespace {

constexpr uint32_t LAUNCH_MAGIC = 0x47414D45;  // "GAME"

enum class Kind : uint8_t { GameBoy = 2 };

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

using GameUi::Contact;

/** Shown when a game cannot start; any touch or the power button returns to the reader. */
[[noreturn]] void showError(const char* line1, const char* line2) {
  FASTEPD& epd = GameDisplay::epd();
  epd.setFont(FONT_12x16);
  epd.fillScreen(BBEP_WHITE);
  GameUi::text(epd, 40, 200, line1);
  if (line2) GameUi::text(epd, 40, 230, line2);
  GameUi::text(epd, 40, 290, "Touch the screen to go back.");
  epd.fullUpdate(CLEAR_FAST, true);
  const uint32_t shown = millis();
  for (;;) {
    gpio.update();
    Contact touched[GameUi::MAX_CONTACTS];
    const bool touch = GameUi::contacts(touched, GameUi::MAX_CONTACTS) > 0 && millis() - shown > 800;
    if (touch || gpio.wasPressed(HalGPIO::BTN_POWER)) exitToReader();
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

}  // namespace

bool pending() { return launchRecord.magic == LAUNCH_MAGIC; }

void launchGame(const char* romPath) { restartWith(Kind::GameBoy, romPath); }

void run() {
  const LaunchRecord launch = launchRecord;
  launchRecord.magic = 0;  // a crash from here on boots back into the reader
  INX_SERIAL.printf("[GAME] game mode: kind=%u %s\n", static_cast<unsigned>(launch.kind), launch.path);
  if (!GameDisplay::begin()) {
    INX_SERIAL.println("[GAME] panel did not power up");
    delay(1000);
    exitToReader();
  }
  GameLight::restore();
  if (launch.kind == Kind::GameBoy) {
    GameBoy::run(launch.path);  // returns only when the game could not start
    showError("This game could not be started:", launch.path);
  }
  exitToReader();
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
