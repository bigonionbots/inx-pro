#pragma once

/**
 * @file SudokuActivity.h
 * @brief Sudoku (Apps -> Sudoku): the puzzle collection's Solo on the e-paper screen.
 *
 * The board fills the middle of the screen; under it are the digits 1-9 and Notes, Erase, Undo,
 * Redo and New. Tap a cell, then a digit. With Notes on, digits go in as pencil marks. New asks
 * for a difficulty and generates a fresh puzzle. The game in progress is saved to the SD card
 * after every move and comes back when the screen opens again. The direction buttons move the
 * cursor and Confirm selects the cell under it.
 */

#if INX_HAS_PUZZLES

#include <array>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "activity/Activity.h"
#include "games/puzzles/PuzzleFrontend.h"

class SudokuActivity final : public Activity {
 public:
  SudokuActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::function<void()> onClose)
      : Activity("Sudoku", renderer, mappedInput), onClose(std::move(onClose)) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;

 private:
  enum class Action : uint8_t { Digit, Notes, Erase, Undo, Redo, New, Level, Restart, Cancel };
  enum class View : uint8_t { Board, Levels };

  struct Target {
    int x, y, w, h;
    Action action;
    int value;
  };

  const std::function<void()> onClose;
  std::unique_ptr<PuzzleFrontend> puzzle;
  View view = View::Board;
  bool notes = false;
  bool hasLastTap = false;
  int lastTapX = 0, lastTapY = 0;
  int statusY = 0;
  int padY = 0;
  int movesSinceFullRefresh = 0;
  std::vector<Target> targets;
  /** Puzzles solved at each level (LEVELS order), kept on the SD card. */
  std::array<int, 5> solvedCount{};
  /** The game on the board has been added to solvedCount (undo and solve again does not count twice). */
  bool solveCounted = false;

  void render(bool fullRefresh);
  void renderStatus();
  void renderPad();
  void renderLevels();
  void button(int x, int y, int w, int h, const char* label, Action action, int value = 0, bool filled = false);
  void run(Action action, int value);
  void startNewGame(int level);
  void afterMove(bool changed);
  void pushChanges();
  std::string levelName() const;
  int levelIndex() const;
  void loadTally();
  void saveTally() const;
  void countSolve();
};

#endif  // INX_HAS_PUZZLES
