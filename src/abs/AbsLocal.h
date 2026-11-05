#pragma once

/**
 * @file AbsLocal.h
 * @brief Audiobookshelf books stored on the SD card.
 *
 * A downloaded book is a folder under /Audiobooks with its audio files and a `.abs.json` that
 * records the server item id, the track order and each track's offset in the whole book. The
 * player uses it to keep the server's track order and to sync progress; `complete` is false
 * until every file is fully downloaded (and the book stays out of the local list until then).
 * Everything here touches the SD card: main loop only, SdIoMutex taken inside.
 */

#if INX_HAS_AUDIO_MODULE

#include <string>
#include <vector>

#include "AbsClient.h"

namespace AbsLocal {

constexpr const char* META_FILE = ".abs.json";

struct BookMeta {
  std::string folder;  ///< /Audiobooks/<title>
  std::string itemId;
  std::string title;
  std::string author;
  uint32_t durationMs = 0;
  bool complete = false;
  std::vector<Abs::Track> tracks;  ///< in play order; filename is the local file in folder
  std::vector<Abs::Chapter> chapters;  ///< the server's, in the whole book (empty in older downloads)
};

bool read(const std::string& folder, BookMeta& out);
bool write(const BookMeta& meta);
/** Finds the downloaded (complete or not) copy of an item. */
bool find(const std::string& itemId, BookMeta& out);
/** Every downloaded item: id and whether its download is complete. One pass over /Audiobooks. */
std::vector<std::pair<std::string, bool>> listDownloaded();
/** A new folder name for an item: the title made safe for FAT, unique under /Audiobooks. */
std::string newFolder(const std::string& title);
/** A track's local file name: the server's file name made safe for FAT. */
std::string localName(const Abs::Track& track);
/** Bytes already on the card for a track (0 when missing). */
uint64_t localSize(const BookMeta& meta, const Abs::Track& track);
/** Deletes the files and the folder. */
bool remove(const BookMeta& meta);

}  // namespace AbsLocal

#endif  // INX_HAS_AUDIO_MODULE
