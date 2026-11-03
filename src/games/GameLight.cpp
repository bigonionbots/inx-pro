/**
 * @file GameLight.cpp
 * @brief Definitions for the game-mode frontlight helper.
 */

#include "GameLight.h"

#if INX_HAS_GAMES

#if INX_HAS_FRONTLIGHT
#include "system/MappedInputManager.h"
// Frontlight.h needs MappedInputManager complete.
#include "system/Frontlight.h"
#endif

namespace GameLight {

#if INX_HAS_FRONTLIGHT

namespace {

frontlight_preferences::Settings settings;
bool loaded = false;
bool changed = false;

void apply() {
  frontlight.setColorTemperature(settings.warmPercent);
  frontlight.setBrightness(settings.brightness);
  if (!settings.enabled) frontlight.off();
}

}  // namespace

void restore() {
  loaded = frontlight_preferences::load(settings);
  if (loaded) apply();
}

void toggle() {
  settings.enabled = settings.enabled ? 0 : 1;
  // Turning on from a stored 0 would stay dark: start at the dimmest lit level.
  if (settings.enabled && settings.brightness == 0) settings.brightness = frontlight_ui::kMinLitPercent;
  apply();
  changed = true;
}

void step(const bool up) {
  const int current = settings.enabled ? settings.brightness : 0;
  const int next = frontlight_ui::stepBrightness(current, up);
  settings.enabled = next > 0 ? 1 : 0;
  if (next > 0) settings.brightness = static_cast<uint8_t>(next);
  apply();
  changed = true;
}

bool isOn() { return settings.enabled != 0; }

uint8_t percent() { return settings.enabled ? settings.brightness : 0; }

void save() {
  if (changed) frontlight_preferences::save(settings);
  changed = false;
}

#else

void restore() {}
void toggle() {}
void step(bool) {}
bool isOn() { return false; }
uint8_t percent() { return 0; }
void save() {}

#endif

}  // namespace GameLight

#endif  // INX_HAS_GAMES
