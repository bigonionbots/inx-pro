#!/usr/bin/env bash
# Host check of the vendored puzzle core (third_party/puzzles) through the firmware's own wrapper
# files: Sudoku, Pattern and Keen generate puzzles at every level the firmware offers (times are
# printed; the ESP32-S3 is roughly 10-20x slower), then save/restore and solve must work.
# Writes build/puzzles_host/board-<puzzle>.pgm, each last board, to check the drawing by eye.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$ROOT_DIR/build/puzzles_host"
mkdir -p "$BUILD_DIR"

cc -std=gnu11 -O2 -w -Werror=implicit-function-declaration -DPUZZLES_HOST_TEST \
  "$ROOT_DIR/test/puzzles_host/puzzles_host_test.c" \
  "$ROOT_DIR"/src/games/puzzles/Puzzle*.c \
  -lm -o "$BUILD_DIR/puzzles_host_test"
"$BUILD_DIR/puzzles_host_test" "$BUILD_DIR/board"
