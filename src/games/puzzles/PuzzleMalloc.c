/**
 * @file PuzzleMalloc.c
 * @brief Compiles the vendored puzzle collection's malloc.c (third_party/puzzles, see VENDORED.md).
 */

#if INX_HAS_PUZZLES || defined(PUZZLES_HOST_TEST)
#define COMBINED
#include "../../../third_party/puzzles/malloc.c"
#endif
