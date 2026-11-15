/**
 * @file PuzzleRandom.c
 * @brief Compiles the vendored puzzle collection's random.c (third_party/puzzles, see VENDORED.md).
 */

#if INX_HAS_PUZZLES || defined(PUZZLES_HOST_TEST)
#define COMBINED
#include "../../../third_party/puzzles/random.c"
#endif
