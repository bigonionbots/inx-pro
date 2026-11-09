#pragma once

/**
 * @file AbsClient.h
 * @brief Audiobookshelf (ABS) API client: sign-in, token refresh, library browsing, progress.
 *
 * Sign-in follows the v2.26+ JWT scheme: POST /login with x-return-tokens returns an access token
 * (short-lived) and a refresh token (rotates on every use, 30 days unused by default). Requests
 * carry the access token; a 401 triggers POST /auth/refresh with x-refresh-token and one retry.
 * Servers older than 2.26 only return the legacy user.token, which is used as the access token.
 * The password is never stored; once the refresh token lapses the user signs in again.
 *
 * Settings live in /.system/audiobookshelf.json. Network calls may run on any task and block;
 * the file is written only by service() on the main loop, because SD access from other tasks
 * would race the reader (see AudioPlayer.h).
 */

#if INX_HAS_AUDIO_MODULE

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "audio/AudioChapter.h"

class AbsIndex;

namespace Abs {

/**
 * esp_http_client buffer sizes for Audiobookshelf requests. Both are over 4 KB on purpose:
 * malloc() serves blocks up to CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL (4096) from internal RAM, and
 * a few clients open at once (stream, sync, covers) took enough of it that USB and TLS failed
 * with out-of-memory errors. Larger ones come from PSRAM.
 */
constexpr int HTTP_RX_BUFFER = 8192;
constexpr int HTTP_TX_BUFFER = 4352;

struct Library {
  std::string id;
  std::string name;
  bool podcasts = false;  ///< a podcast library (mediaType "podcast"), else books
};

/**
 * A podcast episode is addressed as "<library item id>/<episode id>" throughout (shelves, the
 * player, the recent list): that is also the tail of its progress routes (/api/me/progress/...).
 */
inline bool isEpisode(const std::string& id) { return id.find('/') != std::string::npos; }
/** The library item of an id: the podcast of an episode, else the id itself (covers are per item). */
inline std::string itemOf(const std::string& id) { return id.substr(0, id.find('/')); }

struct ItemSummary {
  std::string id;
  std::string title;
  std::string author;
  uint32_t durationSec = 0;
  uint64_t sizeBytes = 0;
  float progress = 0.0f;  ///< 0..1 from the user's media progress
  bool finished = false;
};

/** One episode of a podcast, for its episode list. */
struct Episode {
  std::string id;  ///< the episode's own id (see isEpisode() for the combined one)
  std::string title;
  uint64_t publishedMs = 0;  ///< epoch ms, 0 if unknown
  uint32_t durationSec = 0;
  float progress = 0.0f;
  bool finished = false;
};

/** A podcast and its newest episodes (GET /api/items/:id?expanded=1). */
struct Podcast {
  std::string id;
  std::string title;
  std::string author;
  uint32_t episodeCount = 0;      ///< all of them, also those past the list's limit
  std::vector<Episode> episodes;  ///< newest first
};

/**
 * A home-page shelf (GET /api/libraries/:id/personalized): of books, of podcasts, or of
 * episodes (their ids combined, isEpisode(); title the episode's, author the podcast's).
 */
struct Shelf {
  std::string id;     ///< continue-listening, continue-series, recently-added, newest-episodes, ...
  std::string label;  ///< as the server names it, e.g. "Continue Listening"
  std::vector<ItemSummary> items;
};

struct Track {
  int index = 0;
  std::string contentUrl;  ///< server-relative, /api/items/<id>/file/<ino>
  std::string filename;
  uint64_t sizeBytes = 0;
  uint32_t startOffsetMs = 0;  ///< position of this track in the whole book
  uint32_t durationMs = 0;
};

using Chapter = AudioChapter;

struct ItemDetail {
  std::string id;
  std::string title;
  std::string author;
  uint32_t durationMs = 0;
  std::vector<Track> tracks;
  std::vector<Chapter> chapters;
};

struct Progress {
  bool exists = false;
  uint32_t currentMs = 0;  ///< in the whole book
  uint32_t durationMs = 0;
  bool finished = false;
  uint64_t lastUpdateMs = 0;  ///< server epoch ms
};

/** Loads the settings file. Main loop (SD). */
void begin();
/** Persists rotated tokens. Main loop (SD). */
void service();

bool configured();  ///< a server URL and a user are set
bool signedIn();    ///< a refresh or legacy token is held
std::string serverUrl();
std::string username();

/** Signs in and keeps the tokens. Network; any task. The settings are saved by service(). */
bool signIn(const std::string& server, const std::string& user, const std::string& password, std::string& error);
/** Forgets the tokens (and tells the server). */
void signOut();

bool libraries(std::vector<Library>& out, std::string& error);
/** One page of a book library, sorted by title. */
bool items(const std::string& libraryId, int page, int perPage, std::vector<ItemSummary>& out, int& total,
           std::string& error);
/**
 * The library's home shelves of books, up to `perShelf` each, in the server's order, with the
 * user's progress. Shelves of other things (series, authors) and empty ones are left out.
 */
bool shelves(const std::string& libraryId, int perShelf, std::vector<Shelf>& out, std::string& error);
/** A book, or (combined id, isEpisode()) one podcast episode as a book of one track. */
bool item(const std::string& itemId, ItemDetail& out, std::string& error);
/** A podcast with its newest `maxEpisodes` episodes and the user's progress in them. */
bool podcast(const std::string& itemId, size_t maxEpisodes, Podcast& out, std::string& error);
/**
 * The whole library into `out` (finished), with the user's progress: the items list page by page
 * (GET /api/libraries/:id/items, minified) and /api/me. Any task; no SD.
 */
bool libraryIndex(const std::string& libraryId, AbsIndex& out, std::string& error);
/** Progress routes take a combined episode id as is. */
bool progress(const std::string& itemId, Progress& out, std::string& error);
bool updateProgress(const std::string& itemId, uint32_t currentMs, uint32_t durationMs, bool finished,
                    std::string& error);

/** Absolute URL for a server-relative path such as a track's contentUrl. */
std::string url(const std::string& path);
/** "Bearer <access token>" for requests made outside this client (HttpAudioFile). */
std::string authorization();
/**
 * Why a request could not reach `url`, for the screen: from esp_http_client's error and esp-tls's
 * last error (esp_http_client_get_and_clear_last_tls_error: its result, the mbedTLS code and the
 * certificate flags). Names the cause when it can (name not found, unreachable, certificate).
 */
std::string describeConnectFailure(int err, int tlsErr, int mbedtlsCode, int certFlags, const std::string& url);
/**
 * While one exists, requests from the task that made it reuse one HTTPS connection (keep-alive)
 * instead of a TLS handshake each, about a second through a tunnel; the connection closes with
 * it, so it holds no memory in between. For a short run of requests (a progress sync's check,
 * save and read-back). One at a time.
 */
class KeepConnection {
 public:
  KeepConnection();
  ~KeepConnection();
  KeepConnection(const KeepConnection&) = delete;
  KeepConnection& operator=(const KeepConnection&) = delete;
};
/** Renews the access token after a 401. Network; any task. */
bool refreshAccess();

}  // namespace Abs

#endif  // INX_HAS_AUDIO_MODULE
