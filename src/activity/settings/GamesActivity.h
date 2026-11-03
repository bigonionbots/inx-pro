#pragma once

/**
 * @file GamesActivity.h
 * @brief Public interface and types for GamesActivity.
 */

#if INX_HAS_GAMES

#include <functional>
#include <string>
#include <vector>

#include "activity/Activity.h"

/**
 * Games screen (Home -> Shortcuts -> Games). Lists the ROMs under /Games and launches game mode
 * (games/GameMode.h), which restarts the device into a boot that drives the panel directly.
 * Milestone 1 offers the panel and touch test only; the emulators follow.
 */
class GamesActivity final : public Activity {
 public:
  GamesActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::function<void()> onClose)
      : Activity("Games", renderer, mappedInput), onClose(std::move(onClose)) {}

  void onEnter() override;
  void loop() override;

 private:
  const std::function<void()> onClose;
  std::vector<std::string> roms;
  bool updateRequired = false;
  int testButtonY = 0;

  void render();
};

#endif  // INX_HAS_GAMES
