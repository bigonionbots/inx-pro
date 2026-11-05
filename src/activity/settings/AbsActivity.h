#pragma once

/**
 * @file AbsActivity.h
 * @brief Public interface and types for AbsActivity.
 */

#if INX_HAS_AUDIO_MODULE

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <atomic>
#include <functional>
#include <string>
#include <vector>

#include "abs/AbsClient.h"
#include "abs/AbsIndex.h"
#include "activity/ActivityWithSubactivity.h"

/**
 * Audiobookshelf browser (Audiobooks -> Books -> Audiobookshelf): sign in, pick a library and a
 * book, then stream it or download it for offline listening. Downloads run only while the
 * Download view is showing (see AbsDownloader).
 *
 * Offline first: the libraries, the home shelves and the whole library index (AbsIndex) are
 * cached on the card (AbsLibrary), so the screen shows them at once, browses A-Z / by author /
 * by search without the network, and opens downloaded books offline. When Wi-Fi is up, a
 * background refresh fetches them again into staging copies, swapped in on the main loop; only a
 * first visit (nothing cached) waits on "Loading".
 */
class AbsActivity final : public ActivityWithSubactivity {
 public:
  AbsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::function<void()> onClose,
              std::function<void()> onPlay)
      : ActivityWithSubactivity("Audiobookshelf", renderer, mappedInput),
        onClose(std::move(onClose)),
        onPlay(std::move(onPlay)) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  bool preventAutoSleep() override;

 private:
  /** Home: the library's shelves (Continue Listening, Recently Added, ...), the start view. */
  /**
   * Home: the shelves, the start view. List: books from the index (A-Z, one author's, search
   * results). Authors: the index's authors. Letters: the A-Z jump grid. More: libraries,
   * refresh, sign out.
   */
  enum class View : uint8_t { SignIn, More, List, Detail, Download, Home, Authors, Letters };
  enum class ListKind : uint8_t { Az, Author, Search };
  enum class Action : uint8_t {
    EditServer,
    EditUser,
    EditPassword,
    SignIn,
    WifiSettings,
    RetryWifi,
    SignOut,
    OpenLibrary,
    OpenBook,       ///< index = position in listBooks
    OpenShelfBook,  ///< index = shelf * SHELF_INDEX + row
    ShowAz,
    ShowAuthors,
    OpenAuthor,  ///< index = author number
    ShowSearch,
    ShowLetters,
    PickLetter,  ///< index = the letter ('#' or 'A'..'Z')
    ShowMore,
    Refresh,
    PageUp,
    PageDown,
    Stream,
    Download,
    PlayLocal,
    DeleteLocal,
    PauseDownload,
  };
  struct Target {
    int x, y, w, h;
    Action action;
    int index;
  };

  const std::function<void()> onClose;
  const std::function<void()> onPlay;

  View view = View::SignIn;
  std::vector<Target> targets;
  bool updateRequired = false;
  unsigned long lastRenderMs = 0;

  // Sign-in form
  std::string server;
  std::string user;
  std::string password;

  // Data, written by the worker while busy and read by the UI once it is done
  SemaphoreHandle_t dataLock = nullptr;
  std::atomic<bool> busy{false};
  std::atomic<bool> resultReady{false};
  std::atomic<bool> wifiHeld{false};
  /** What to do once the Wi-Fi picker has connected (it was opened because Wi-Fi was down). */
  std::function<void()> pending;
  bool wifiNeeded = false;  ///< the picker was cancelled: show only the message and Connect
  std::string busyText;
  std::string error;
  std::vector<Abs::Library> libraries;
  Abs::Library library;
  std::vector<Abs::Shelf> shelves;
  AbsIndex index;                ///< the whole library (cached; refreshed in the background)
  View detailFrom = View::List;  ///< where Back from a book goes: the view it was opened from

  // Background refresh: the worker fills these, the main loop swaps them in (applyRefresh).
  std::atomic<bool> refreshing{false};
  std::atomic<bool> refreshReady{false};
  std::vector<Abs::Library> stagedLibraries;
  std::vector<Abs::Shelf> stagedShelves;
  AbsIndex stagedIndex;
  bool stagedHasIndex = false;
  std::string refreshError;
  bool offline = false;  ///< showing the cache because Wi-Fi is down

  // List view
  ListKind listKind = ListKind::Az;
  std::string listKey;  ///< the author's name, or the search
  AbsIndex::PsramVector<uint32_t> listBooks;
  int listFirst = 0;
  int authorsFirst = 0;
  int contentTop = 120;                ///< where views start under the header, from the last render
  TaskHandle_t refreshTask = nullptr;  ///< suspends itself when done; applyRefresh() deletes it
  Abs::ItemDetail detail;
  Abs::Progress serverProgress;
  bool localCopy = false;
  bool localComplete = false;
  std::vector<std::pair<std::string, bool>> downloaded;  ///< item id, complete

  void run(Action action, int index);
  void showView(View next);
  /** Required: opens the Wi-Fi picker first when Wi-Fi is down. BestEffort: runs anyway (sign-out). */
  enum class Network : uint8_t { Required, BestEffort };
  void startWork(const char* text, std::function<void(std::string& error)> work, View nextView,
                 Network network = Network::Required);
  /** Opens the Wi-Fi picker; `then` runs once it connects. */
  void requestWifi(std::function<void()> then);
  void loadLibraries();
  void loadHome();
  /** Shows the cached library (true), or false when nothing is cached for it. */
  bool showCached();
  /** Fetches libraries, shelves and (if stale or `force`) the index in the background. */
  void startRefresh(bool force);
  void applyRefresh();
  void deleteRefreshTask();
  void openList(ListKind kind, const std::string& key);
  void rebuildList();
  void openBook(const std::string& itemId, View from);
  void loadDetail(const std::string& itemId);
  void refreshLocalState();
  void editField(const char* title, std::string& field, bool secret);

  void render();
  void renderSignIn(int y);
  void renderMore(int y);
  void renderList(int y);
  void renderAuthors(int y);
  void renderLetters(int y);
  void pager(int y, bool previous, bool next, bool jump);
  void renderHome(int y);
  void renderDetail(int y);
  void renderDownload(int y);
  void button(int x, int y, int w, int h, const char* label, Action action, int index = 0, bool filled = false);
  /** The item's cached cover, or a placeholder (noting it for AbsCovers::request()). */
  void drawCover(const std::string& itemId, int x, int y, int size);
  std::vector<std::string> missingCovers;  ///< during render()
  uint32_t renderedCovers = 0;             ///< AbsCovers::changeCounter() at the last render
  void row(int y, int height, Action action, int index);
  int rowsPerPage(int rowHeight) const;
};

#endif  // INX_HAS_AUDIO_MODULE
