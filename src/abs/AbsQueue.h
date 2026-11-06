#pragma once

/**
 * @file AbsQueue.h
 * @brief The Audiobookshelf download queue: books wait their turn and download in the
 *        background while the T5 is on a charger (or at once, when asked to start now).
 *
 * The queue is saved in /.system/abs_queue.json, in order. service() runs every main loop (from
 * main.cpp), whatever screen is open: it writes the downloader's buffers to the card
 * (AbsDownloader::service()), checks the charger every few seconds (BQ25896 external VBUS, so
 * the boost that powers the audio module does not count), starts the first book allowed to run,
 * pauses a charger-only book when the charger goes, and drops a book from the queue once its
 * download is complete. A book's details (its tracks) are fetched on a task of the queue's own;
 * the downloader then reconnects Wi-Fi with the saved networks itself.
 *
 * keepAwake(): on the charger, the T5 stays awake until the queue is done, then sleeps as usual.
 * Main loop only, except where noted.
 */

#if INX_HAS_AUDIO_MODULE

#include <cstdint>
#include <string>
#include <vector>

namespace AbsQueue {

enum class State : uint8_t {
  Waiting,      ///< for the charger (or for its turn)
  Starting,     ///< fetching its details
  Downloading,  ///< the downloader is on it
  Failed,       ///< stopped with an error; retried after a while, or by Start now
};

struct Entry {
  std::string itemId;
  std::string title;
  std::string author;
  bool now = false;  ///< Start now: downloads on battery too
  State state = State::Waiting;
  std::string error;
};

/** Loads the queue. Call once at boot, after the SD card. */
void begin();

/** Every main loop. */
void service();

/** True while the T5 is on a charger and the queue has work: the sleep timer waits. */
bool keepAwake();

/** Before deep sleep: pauses a running download (it resumes from the card later). */
void shutdown();

/** The queue in order, with each book's state. */
std::vector<Entry> entries();

bool contains(const std::string& itemId);

/** Adds a book at the end (no-op if queued). `now`: may download on battery. */
void add(const std::string& itemId, const std::string& title, const std::string& author, bool now = false);

/** Takes a book out of the queue; stops its download and deletes what it had downloaded. */
void remove(const std::string& itemId);

/** One place earlier or later in the queue. A running download keeps running. */
void move(const std::string& itemId, int delta);

/** To the front, allowed on battery, and (if another book is downloading) next in line. */
void startNow(const std::string& itemId);

/** Failed books try again at once (Wi-Fi is back, the charger plugged in), not in 10 minutes. */
void retryFailed();

/**
 * While held, the queue starts nothing new (a running download carries on): the Wi-Fi picker is
 * open, and the queue connecting with the saved networks meanwhile would fight it for the radio.
 */
void holdOff(bool hold);

/** On a charger (external power on VBUS), as last checked. */
bool charging();

/** Increments whenever the queue or a state in it changes, so screens know to redraw. */
uint32_t changeCounter();

}  // namespace AbsQueue

#endif  // INX_HAS_AUDIO_MODULE
