#pragma once

/**
 * @file AudioChapter.h
 * @brief A chapter of an audiobook: where it starts in the whole book, and its name.
 *
 * Shared by the MP4 reader (chapters in .m4b files), the Audiobookshelf client (the server's
 * chapter list) and the player.
 */

#include <cstdint>
#include <string>

struct AudioChapter {
  uint32_t startMs = 0;  ///< from the start of the book
  std::string title;
};
