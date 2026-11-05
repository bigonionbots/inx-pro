#pragma once

/**
 * @file AbsLibrary.h
 * @brief The Audiobookshelf screen's data cached on the SD card, so it opens at once and browses
 *        offline: the libraries, each library's home shelves and its whole index (AbsIndex).
 *
 * Files in /.system/cache/abs/: libraries.json, <library id>-home.json, <library id>.idx. All
 * written through a temporary file. Main loop only (SD); fetching is AbsClient's, on a task.
 */

#if INX_HAS_AUDIO_MODULE

#include <string>
#include <vector>

#include "AbsClient.h"
#include "AbsIndex.h"

namespace AbsLibrary {

bool loadLibraries(std::vector<Abs::Library>& out);
void saveLibraries(const std::vector<Abs::Library>& libraries);

bool loadShelves(const std::string& libraryId, std::vector<Abs::Shelf>& out);
void saveShelves(const std::string& libraryId, const std::vector<Abs::Shelf>& shelves);

bool loadIndex(const std::string& libraryId, AbsIndex& out);
void saveIndex(const std::string& libraryId, const AbsIndex& index);

/** Forgets everything (signing out). */
void clear();

}  // namespace AbsLibrary

#endif  // INX_HAS_AUDIO_MODULE
