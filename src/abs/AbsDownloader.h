#pragma once

/**
 * @file AbsDownloader.h
 * @brief Downloads an Audiobookshelf book into /Audiobooks for offline playback. Resumable.
 *
 * A network task reads the tracks through HttpAudioFile into a few PSRAM buffers; service() on
 * the main loop writes them to the card, because SD access from another task would race the
 * reader (see AudioPlayer.h). AbsQueue drives it: it calls service() every main loop (so a
 * download runs in the background, whatever screen is open), starts the next queued book and
 * pauses one when the charger goes. pause() stops it, and start() again continues from the bytes
 * already on the card.
 */

#if INX_HAS_AUDIO_MODULE

#include <cstdint>
#include <string>

#include "AbsClient.h"

namespace AbsDownloader {

struct Status {
  bool active = false;  ///< network task running or buffers still being written
  bool done = false;
  bool failed = false;
  std::string error;
  std::string itemId;
  std::string title;
  uint64_t doneBytes = 0;
  uint64_t totalBytes = 0;
  int track = 0;  ///< 0-based, the one being written
  int trackCount = 0;
  uint32_t bytesPerSecond = 0;
};

/** Starts or resumes. Main loop. */
bool start(const Abs::ItemDetail& item, std::string& error);
/** Stops (the task ends after its current chunk); what is on the card stays. Main loop. */
void pause();
/** Writes downloaded chunks to the card. Main loop, while the download screen is open. */
void service();
Status status();

}  // namespace AbsDownloader

#endif  // INX_HAS_AUDIO_MODULE
