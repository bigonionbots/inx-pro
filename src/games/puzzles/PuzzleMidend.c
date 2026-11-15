/**
 * @file PuzzleMidend.c
 * @brief Compiles the vendored puzzle collection's midend.c (third_party/puzzles, see VENDORED.md).
 */

#if INX_HAS_PUZZLES || defined(PUZZLES_HOST_TEST)
#define COMBINED
#include "../../../third_party/puzzles/midend.c"
#endif
