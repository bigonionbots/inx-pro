/**
 * @file PuzzleDivvy.c
 * @brief Compiles the vendored puzzle collection's divvy.c (third_party/puzzles, see VENDORED.md).
 */

#if INX_HAS_PUZZLES || defined(PUZZLES_HOST_TEST)
#define COMBINED
#include "../../../third_party/puzzles/divvy.c"
#endif
