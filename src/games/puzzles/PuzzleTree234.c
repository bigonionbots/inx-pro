/**
 * @file PuzzleTree234.c
 * @brief Compiles the vendored puzzle collection's tree234.c (third_party/puzzles, see VENDORED.md).
 */

#if INX_HAS_PUZZLES || defined(PUZZLES_HOST_TEST)
#define COMBINED
#include "../../../third_party/puzzles/tree234.c"
#endif
