/**
 * @file video_row_test.cpp
 * @brief Host test of GameVideoRow::prepare(): each colour change gets exactly `passes` drives.
 */

#include <cassert>
#include <cstdio>

#include "../../src/games/GameVideoRow.h"

int main() {
  static const GameVideoRow::CodeTables tables;
  constexpr int BYTES = 4;
  for (int passes = 1; passes <= GameVideoRow::MAX_PASSES; ++passes) {
    uint8_t target[BYTES], shown[BYTES], c0[BYTES] = {}, c1[BYTES] = {}, c2[BYTES] = {}, codes[2 * BYTES];
    std::memset(target, 0xFF, BYTES);
    std::memset(shown, 0xFF, BYTES);
    target[0] = 0x7F;  // pixel 0 -> black
    target[3] = 0xFE;  // pixel 31 -> black
    int drives0 = 0, drives31 = 0;
    for (int scan = 0; scan < 16; ++scan) {
      if (scan == 4) target[0] = 0xFF;  // pixel 0 back to white mid-way
      GameVideoRow::prepare(tables, BYTES, target, shown, c0, c1, c2, codes, passes);
      const int code0 = codes[0] >> 6;
      const int code31 = codes[7] & 3;
      if (code0) ++drives0;
      if (code31) {
        assert(code31 == 1);
        ++drives31;
      }
      for (int i = 0; i < 2 * BYTES; ++i) {
        if (i != 0 && i != 7) assert(codes[i] == 0);
      }
      if (scan >= 4 && scan < 4 + passes) assert(code0 == 2);
    }
    const int expect0 = (passes < 4 ? passes : 4) + passes;
    if (drives0 != expect0) std::fprintf(stderr, "passes %d: drives0 %d expected %d\n", passes, drives0, expect0);
    assert(drives0 == expect0);
    assert(drives31 == passes);
  }
  std::puts("GameVideoRow: ok");
  return 0;
}
