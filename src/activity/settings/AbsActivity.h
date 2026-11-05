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
#include "activity/ActivityWithSubactivity.h"

/**
 * Audiobookshelf browser (Audiobooks -> Books -> Audiobookshelf): sign in, pick a library and a
 * book, then stream it or download it for offline listening. Network calls run on a worker task
 * while the screen shows "Loading"; Wi-Fi stays connected while the screen is open. Downloads run
 * only while the Download view is showing (see AbsDownloader).
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
  enum class View : uint8_t { SignIn, Libraries, Books, Detail, Download, Home };
  enum class Action : uint8_t {
    EditServer,
    EditUser,
    EditPassword,
    SignIn,
    WifiSettings,
    RetryWifi,
    SignOut,
    OpenLibrary,
    OpenBook,
    OpenShelfBook,  ///< index = shelf * SHELF_INDEX + row
    AllBooks,
    OpenLibraries,
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
  /** Network work waiting for the Wi-Fi picker (startWork() found Wi-Fi down). */
  struct PendingWork {
    std::string text;
    std::function<void(std::string& error)> work;
    View nextView = View::SignIn;
  };
  PendingWork pending;
  bool hasPending = false;
  bool wifiNeeded = false;  ///< the picker was cancelled: show only the message and Connect
  std::string busyText;
  std::string error;
  std::vector<Abs::Library> libraries;
  Abs::Library library;
  std::vector<Abs::ItemSummary> books;
  std::vector<Abs::Shelf> shelves;
  View detailFrom = View::Books;  ///< where Back from a book goes: the list it was opened from
  int page = 0;
  int total = 0;
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
  void requestWifi();
  void loadLibraries();
  void loadBooks();
  void loadHome();
  void loadDetail(const std::string& itemId);
  void refreshLocalState();
  void editField(const char* title, std::string& field, bool secret);

  void render();
  void renderSignIn(int y);
  void renderLibraries(int y);
  void renderBooks(int y);
  void renderHome(int y);
  void renderDetail(int y);
  void renderDownload(int y);
  void button(int x, int y, int w, int h, const char* label, Action action, int index = 0, bool filled = false);
  /** The item's cached cover, or a placeholder (noting it for AbsCovers::request()). */
  void drawCover(const std::string& itemId, int x, int y, int size);
  std::vector<std::string> missingCovers;  ///< during render()
  uint32_t renderedCovers = 0;             ///< AbsCovers::changeCounter() at the last render
  void row(int y, int height, Action action, int index);
  int booksPerPage() const;
};

#endif  // INX_HAS_AUDIO_MODULE
