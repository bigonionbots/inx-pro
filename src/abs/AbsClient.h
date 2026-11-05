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

#include <cstdint>
#include <string>
#include <vector>

namespace Abs {

struct Library {
  std::string id;
  std::string name;
};

struct ItemSummary {
  std::string id;
  std::string title;
  std::string author;
  uint32_t durationSec = 0;
  uint64_t sizeBytes = 0;
  float progress = 0.0f;  ///< 0..1 from the user's media progress
  bool finished = false;
};

struct Track {
  int index = 0;
  std::string contentUrl;  ///< server-relative, /api/items/<id>/file/<ino>
  std::string filename;
  uint64_t sizeBytes = 0;
  uint32_t startOffsetMs = 0;  ///< position of this track in the whole book
  uint32_t durationMs = 0;
};

struct Chapter {
  uint32_t startMs = 0;
  std::string title;
};

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
bool item(const std::string& itemId, ItemDetail& out, std::string& error);
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
/** Renews the access token after a 401. Network; any task. */
bool refreshAccess();

}  // namespace Abs

#endif  // INX_HAS_AUDIO_MODULE
