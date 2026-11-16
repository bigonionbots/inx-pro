/**
 * @file PuzzleMatching.c
 * @brief Compiles the vendored puzzle collection's matching.c (third_party/puzzles, see VENDORED.md).
 */

#if INX_HAS_PUZZLES || defined(PUZZLES_HOST_TEST)
#define COMBINED
#include "../../../third_party/puzzles/matching.c"
#endif
