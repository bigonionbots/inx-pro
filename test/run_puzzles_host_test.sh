#!/usr/bin/env bash
# Host check of the vendored puzzle core (third_party/puzzles) through the firmware's own wrapper
# files: Solo generates puzzles at every difficulty the firmware offers (times are printed; the
# ESP32-S3 is roughly 10-20x slower), then a move, save/restore and solve must work.
# Writes build/puzzles_host/solo.pgm, the last board, to check the drawing by eye.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$ROOT_DIR/build/puzzles_host"
mkdir -p "$BUILD_DIR"

cc -std=gnu11 -O2 -w -Werror=implicit-function-declaration -DPUZZLES_HOST_TEST \
  "$ROOT_DIR/test/puzzles_host/solo_host_test.c" \
  "$ROOT_DIR"/src/games/puzzles/Puzzle*.c \
  -lm -o "$BUILD_DIR/solo_host_test"
"$BUILD_DIR/solo_host_test" "$BUILD_DIR/solo.pgm"
