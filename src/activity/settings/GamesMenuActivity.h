#pragma once

/**
 * @file GamesMenuActivity.h
 * @brief The Games menu (Apps -> Games, Home's Games shortcut): one row per game this build has.
 *
 * Game Boy (the ROM list, GamesActivity; t5s3_audio only) and the puzzles Sudoku, Pattern and Keen
 * (PuzzleActivity; every device). Each opens full screen and comes back here; Back here goes Home. New games go in
 * kGames in the .cpp.
 */

#if INX_HAS_GAMES || INX_HAS_PUZZLES

#include <functional>

#include "activity/Activity.h"

class GamesMenuActivity final : public Activity {
 public:
  GamesMenuActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::function<void()> onClose)
      : Activity("GamesMenu", renderer, mappedInput), onClose(std::move(onClose)) {}

  void onEnter() override;
  void loop() override;

 private:
  const std::function<void()> onClose;
  int selected = 0;
  int listTop = 0;
  int rowHeight = 0;

  void render();
  void open(int index);
};

#endif  // INX_HAS_GAMES || INX_HAS_PUZZLES
