/**
 * @file PuzzleKeen.c
 * @brief Compiles the vendored puzzle collection's keen.c (third_party/puzzles, see VENDORED.md).
 */

#if INX_HAS_PUZZLES || defined(PUZZLES_HOST_TEST)
#define COMBINED
#include "../../../third_party/puzzles/keen.c"
#endif
