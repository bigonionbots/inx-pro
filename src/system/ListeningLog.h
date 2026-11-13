#pragma once

/**
 * @file ListeningLog.h
 * @brief Audiobook listening time: local stats for every book, and Audiobookshelf sessions to upload.
 */

#if INX_HAS_AUDIO_MODULE

#include <cstdint>
#include <string>
#include <vector>

/**
 * The player reports listened time here (record(), main loop, every save). Two things come of it:
 * - Local stats (/.system/audio_stats.json): total, per day and per book, for every audiobook,
 *   including books that are only on the card.
 * - For Audiobookshelf books, listening sessions (/.system/abs_sessions.json) that the progress sync
 *   uploads with POST /api/session/local, as the apps do for offline playback, so the server's
 *   listening stats count the T5. A session is one stretch of listening to one book on one day; a
 *   session sent again replaces its listening time on the server, so an upload that fails is simply
 *   sent again later, Wi-Fi off or on.
 *
 * Days need the clock (the RTC, in local time; UTC from the time zone setting). Without a valid
 * date, time still counts in the total but no day or session gets it.
 */
namespace ListeningLog {

/** What the player knows of the book being listened to. */
struct BookInfo {
  std::string key;     ///< stats key: the Audiobookshelf item id (podcast: "<item>/<episode>"), else the path
  std::string itemId;  ///< "" for a book that is only on the card (no session)
  std::string title;
  std::string author;
  uint32_t positionMs = 0;  ///< in the whole book, now
  uint32_t durationMs = 0;  ///< 0 if unknown
};

/** Adds listened time to the book (main loop; reads the clock and writes the card). */
void record(const BookInfo& book, uint32_t listenedMs);
/** The book stopped (closed, another opened): its next listening starts a new session. Main loop. */
void endSession();

/** A session ready to upload: its id, JSON body, and the listening time that body carries. */
struct Upload {
  std::string id;
  std::string itemId;
  std::string body;
  uint32_t listenedMs = 0;
};
/** Sessions with listening time the server has not seen yet (at most a few, oldest first). Main loop. */
std::vector<Upload> pendingUploads();
/** The server took this session with this much listening time. Main loop. */
void uploaded(const std::string& id, uint32_t listenedMs);

struct DayTotal {
  uint32_t dateKey;  ///< YYYYMMDD, local
  uint32_t seconds;
};
struct BookTotal {
  std::string key;
  std::string title;
  std::string author;
  uint32_t seconds = 0;
  uint32_t lastDateKey = 0;  ///< YYYYMMDD of the last listening, 0 if unknown
};
/** Local stats (main loop): every book listened to on the T5. */
struct Stats {
  uint64_t totalSeconds = 0;
  std::vector<DayTotal> days;    ///< oldest first, the last DAYS_KEPT days with listening
  std::vector<BookTotal> books;  ///< most recently listened first
};
Stats stats();
/** Today's date as YYYYMMDD from the RTC, 0 if the clock is not set. */
uint32_t todayKey();

}  // namespace ListeningLog

#endif  // INX_HAS_AUDIO_MODULE
