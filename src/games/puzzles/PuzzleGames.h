#pragma once

/**
 * @file PuzzleGames.h
 * @brief The puzzles from the vendored collection that the Games menu offers, and how each one
 *        is played on the e-paper screen (PuzzleActivity).
 *
 * Two kinds of input are supported:
 * - Select, then enter (Sudoku): a tap selects a cell, then a digit on the pad fills it.
 *   The mode button (Notes) makes the tap select for pencil marks instead (the collection's right
 *   click).
 * - Tap to set (selectsCells false): a tap acts on the cell at once; the mode button makes it the
 *   collection's right click.
 *
 * To add a puzzle: vendor it (third_party/puzzles/VENDORED.md), add a PuzzleGame here, its levels
 * to test/puzzles_host/puzzles_host_test.c, and a row to the Games menu.
 */

#if INX_HAS_PUZZLES

#include <cstdint>

struct game;

struct PuzzleLevel {
  const char* name;    ///< "Easy", "10x10"
  const char* params;  ///< the puzzle's own parameter string ("3x3db", "10x10", "6dn")
  uint8_t digits;      ///< digit buttons on the pad, 1..digits (0: no digit row)
};

struct PuzzleGame {
  const char* title;  ///< header and Games menu name
  const game* puzzle;
  const char* saveFile;   ///< the game in progress (the collection's own save format)
  const char* tallyFile;  ///< solved counts per level, then 1 if the game on the board is counted
  const PuzzleLevel* levels;
  uint8_t levelCount;  ///< at most PuzzleGame::MAX_LEVELS
  uint8_t defaultLevel;
  const char* levelPrompt;  ///< heading of the New puzzle menu
  const char* modeLabel;    ///< the mode button: "Notes", "Cross"
  const char* modeHint;     ///< second status line while the mode is on
  bool selectsCells;        ///< select-then-enter (Erase button; the mode re-selects the cell)

  static constexpr int MAX_LEVELS = 6;
};

namespace PuzzleGames {
extern const PuzzleGame SUDOKU;
}  // namespace PuzzleGames

#endif  // INX_HAS_PUZZLES
