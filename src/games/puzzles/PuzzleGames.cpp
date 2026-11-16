/**
 * @file PuzzleGames.cpp
 * @brief Definitions for PuzzleGames: each puzzle's levels, files and controls.
 */

#include "PuzzleGames.h"

#if INX_HAS_PUZZLES

extern "C" {
#define COMBINED
#include "../../../third_party/puzzles/puzzles.h"
#undef COMBINED
}

namespace {

// Kept in step with test/puzzles_host/puzzles_host_test.c, which times generating each one.

/** Solo's parameters: 3x3 blocks, `d` + difficulty letter. */
constexpr PuzzleLevel SUDOKU_LEVELS[] = {
    {"Beginner", "3x3dt", 9}, {"Easy", "3x3db", 9},   {"Medium", "3x3di", 9},
    {"Hard", "3x3da", 9},     {"Expert", "3x3de", 9},
};

/** Pattern's parameters: width x height. 15x15 is about the smallest cell a finger can hit on the T5. */
constexpr PuzzleLevel PATTERN_LEVELS[] = {
    {"5x5", "5x5", 0},
    {"10x10", "10x10", 0},
    {"15x15", "15x15", 0},
};

template <size_t N>
constexpr uint8_t count(const PuzzleLevel (&)[N]) {
  static_assert(N <= PuzzleGame::MAX_LEVELS, "PuzzleActivity keeps a tally for at most MAX_LEVELS levels");
  return static_cast<uint8_t>(N);
}

}  // namespace

namespace PuzzleGames {

const PuzzleGame SUDOKU = {
    "Sudoku",
    &solo,
    "/.system/sudoku.sav",
    "/.system/sudoku-solved.txt",
    SUDOKU_LEVELS,
    count(SUDOKU_LEVELS),
    1,  // Easy
    "New puzzle: choose a difficulty",
    "Notes",
    "Notes on: digits go in as pencil marks",
    true,
};

const PuzzleGame PATTERN = {
    "Pattern",
    &pattern,
    "/.system/pattern.sav",
    "/.system/pattern-solved.txt",
    PATTERN_LEVELS,
    count(PATTERN_LEVELS),
    1,  // 10x10
    "New puzzle: choose a size",
    "Cross",
    "Cross on: taps mark squares as empty",
    false,
};

}  // namespace PuzzleGames

#endif  // INX_HAS_PUZZLES
