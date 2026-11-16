#pragma once

/**
 * @file PuzzleActivity.h
 * @brief One puzzle from the collection (Games -> Sudoku) on the e-paper screen.
 *
 * The board fills the middle of the screen. Under it are the digit buttons (puzzles that take
 * digits) and the tools: the mode button (Notes or Cross), Erase (select-then-enter puzzles),
 * Undo, Redo and New. How taps and the mode work for each puzzle is in PuzzleGames.h. New asks for
 * a level, or clears the board to start the current puzzle over. The game in progress is saved to
 * the SD card after every move and comes back when the screen opens again, and a tally of puzzles
 * solved at each level is kept beside it. The direction buttons move the puzzle's cursor and
 * Confirm acts on the cell under it.
 */

#if INX_HAS_PUZZLES

#include <array>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "activity/Activity.h"
#include "games/puzzles/PuzzleFrontend.h"
#include "games/puzzles/PuzzleGames.h"

class PuzzleActivity final : public Activity {
 public:
  PuzzleActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const PuzzleGame& spec,
                 std::function<void()> onClose)
      : Activity(spec.title, renderer, mappedInput), spec(spec), onClose(std::move(onClose)) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;

 private:
  enum class Action : uint8_t { Digit, Mode, Erase, Undo, Redo, New, Level, Restart, Cancel };
  enum class View : uint8_t { Board, Levels };

  struct Target {
    int x, y, w, h;
    Action action;
    int value;
  };

  /** A copy (a few pointers): switchTo() passes its arguments from a store it frees after the switch. */
  const PuzzleGame spec;
  const std::function<void()> onClose;
  std::unique_ptr<PuzzleFrontend> puzzle;
  View view = View::Board;
  /** The mode button: Notes (pencil marks) or Cross (mark empty); the collection's right click. */
  bool mode = false;
  bool hasLastTap = false;
  int lastTapX = 0, lastTapY = 0;
  int statusY = 0;
  int padY = 0;
  int movesSinceFullRefresh = 0;
  std::vector<Target> targets;
  /** Puzzles solved at each level (spec.levels order), kept on the SD card. */
  std::array<int, PuzzleGame::MAX_LEVELS> solvedCount{};
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
  int levelIndex() const;
  std::string levelName() const;
  /** Digit buttons for the game on the board (its level's, else the default level's). */
  int digitCount() const;
  int padRows() const { return digitCount() > 0 ? 2 : 1; }
  void loadTally();
  void saveTally() const;
  void countSolve();
};

#endif  // INX_HAS_PUZZLES
