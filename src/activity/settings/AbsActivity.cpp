/**
 * @file AbsActivity.cpp
 * @brief Definitions for AbsActivity.
 */

#include "AbsActivity.h"

#if INX_HAS_AUDIO_MODULE

#include <GfxRenderer.h>
#include <ImageRender.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <memory>
#include <numeric>

#include "abs/AbsCovers.h"
#include "abs/AbsDownloader.h"
#include "abs/AbsLibrary.h"
#include "abs/AbsLocal.h"
#include "abs/AbsWifi.h"
#include "activity/network/WifiSelectionActivity.h"
#include "activity/page/SubPage.h"
#include "activity/util/KeyboardEntryActivity.h"
#include "system/AudioPlayer.h"
#include "system/Fonts.h"
#include "system/MappedInputManager.h"
#include "system/UiLayout.h"

namespace {

constexpr int MARGIN = 20;
constexpr int GAP = 12;
constexpr int BUTTON_HEIGHT = 76;
constexpr int ROW_HEIGHT = UiLayout::LIST_ITEM_HEIGHT;
constexpr int BOOK_ROW_HEIGHT = 92;
/** Home shelves: books asked for per shelf, and how a shelf row's index is packed. */
constexpr int SHELF_BOOKS = 4;
constexpr int SHELF_INDEX = 64;
constexpr int SHELF_ROW_HEIGHT = 84;
/** Covers: square, inset in their row; on a book's page, larger beside its details. */
constexpr int ROW_COVER_INSET = 6;
constexpr int DETAIL_COVER = 180;
/** While covers arrive, redraw at most this often (each redraw is a panel refresh). */
constexpr unsigned long COVER_REDRAW_MS = 2000;
constexpr unsigned long DOWNLOAD_REDRAW_MS = 3000;
constexpr uint32_t WORKER_STACK = 12288;  // TLS
/** The index is fetched again (in the background) at most this often, unless asked to. */
constexpr unsigned long INDEX_REFRESH_MS = 10 * 60 * 1000;

// Across visits to the screen (this boot): the library last opened, and when its index was fetched.
std::string lastLibraryId;
std::string indexFetchedFor;
unsigned long indexFetchedMs = 0;

std::string formatDuration(const uint32_t seconds) {
  char text[32];
  if (seconds >= 3600) {
    snprintf(text, sizeof(text), "%lu h %lu min", static_cast<unsigned long>(seconds / 3600),
             static_cast<unsigned long>(seconds / 60 % 60));
  } else {
    snprintf(text, sizeof(text), "%lu min", static_cast<unsigned long>(seconds / 60));
  }
  return text;
}

std::string formatMb(const uint64_t bytes) {
  char text[24];
  snprintf(text, sizeof(text), "%.1f MB", bytes / (1024.0 * 1024.0));
  return text;
}

struct WorkerJob {
  std::function<void()> body;
};

void workerMain(void* raw) {
  std::unique_ptr<WorkerJob> job(static_cast<WorkerJob*>(raw));
  job->body();
  job.reset();
  vTaskDelete(nullptr);
}

}  // namespace

void AbsActivity::onEnter() {
  ActivityWithSubactivity::onEnter();
  dataLock = xSemaphoreCreateMutex();
  AbsWifi::prepare();
  server = Abs::serverUrl();
  user = Abs::username();
  if (Abs::signedIn()) {
    if (showCached()) {
      resumeIfAsked();
    } else {
      loadLibraries();  // first visit: nothing cached, so wait for the server (resumes after)
    }
  } else {
    showView(View::SignIn);
  }
}

bool AbsActivity::showCached() {
  if (!AbsLibrary::loadLibraries(libraries)) return false;
  const auto last =
      std::find_if(libraries.begin(), libraries.end(), [](const Abs::Library& l) { return l.id == lastLibraryId; });
  if (last != libraries.end()) {
    library = *last;
  } else if (libraries.size() == 1) {
    library = libraries.front();
  } else {
    showView(View::More);  // several libraries and none chosen yet
    startRefresh(false);
    return true;
  }
  shelves.clear();
  AbsLibrary::loadShelves(library.id, shelves);
  index.clear();
  AbsLibrary::loadIndex(library.id, index);
  showView(View::Home);
  startRefresh(false);
  return true;
}

void AbsActivity::startRefresh(const bool force) {
  if (refreshing || refreshTask || !Abs::signedIn()) return;
  if (!AbsWifi::connected()) {
    offline = true;  // browse the cache; Refresh (under More) asks for Wi-Fi
    updateRequired = true;
    return;
  }
  offline = false;
  const bool wantIndex = !library.id.empty() && (force || index.empty() || indexFetchedFor != library.id ||
                                                 millis() - indexFetchedMs > INDEX_REFRESH_MS);
  refreshing = true;
  refreshError.clear();
  stagedHasIndex = false;
  updateRequired = true;
  auto* job = new WorkerJob();
  job->body = [this, libraryId = library.id, wantIndex] {
    std::string failure;
    {
      AbsWifi::Use wifi(failure);
      if (wifi.ok() && Abs::libraries(stagedLibraries, failure) && !libraryId.empty() &&
          Abs::shelves(libraryId, SHELF_BOOKS, stagedShelves, failure) && wantIndex) {
        stagedHasIndex = Abs::libraryIndex(libraryId, stagedIndex, failure);
      }
    }
    refreshError = failure;
    refreshReady = true;
    refreshing = false;
  };
  // A PSRAM stack: the index fetch runs alongside the screen, and internal RAM is short.
  if (xTaskCreatePinnedToCoreWithCaps(
          [](void* raw) {
            {
              std::unique_ptr<WorkerJob> j(static_cast<WorkerJob*>(raw));
              j->body();
            }
            // A task with a PSRAM stack should not delete itself: the main loop deletes it once
            // it is suspended (deleteRefreshTask()).
            vTaskSuspend(nullptr);
          },
          "abs_refresh", WORKER_STACK, job, 1, &refreshTask, tskNO_AFFINITY,
          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
    delete job;
    refreshTask = nullptr;
    refreshing = false;
  }
}

/** Deletes the finished refresh task, once it has really suspended itself (it is just finishing). */
void AbsActivity::deleteRefreshTask() {
  if (!refreshTask) return;
  for (int i = 0; i < 200 && eTaskGetState(refreshTask) != eSuspended; ++i) vTaskDelay(pdMS_TO_TICKS(5));
  vTaskDeleteWithCaps(refreshTask);
  refreshTask = nullptr;
}

void AbsActivity::applyRefresh() {
  deleteRefreshTask();
  if (!refreshError.empty()) {
    // Keep showing the cache; More says why it did not update.
    stagedLibraries.clear();
    stagedShelves.clear();
    stagedIndex.clear();
    updateRequired = true;
    return;
  }
  if (!stagedLibraries.empty()) {
    libraries = std::move(stagedLibraries);
    AbsLibrary::saveLibraries(libraries);
    for (const Abs::Library& l : libraries) {
      if (l.id == library.id) library.name = l.name;
    }
  }
  if (!library.id.empty()) {
    shelves = std::move(stagedShelves);
    AbsLibrary::saveShelves(library.id, shelves);
  }
  if (stagedHasIndex) {
    std::swap(index, stagedIndex);
    stagedIndex.clear();
    AbsLibrary::saveIndex(library.id, index);
    indexFetchedFor = library.id;
    indexFetchedMs = millis();
    rebuildList();  // the book numbers changed with the index
  }
  stagedLibraries.clear();
  stagedShelves.clear();
  updateRequired = true;
}

void AbsActivity::openList(const ListKind kind, const std::string& key) {
  listKind = kind;
  listKey = key;
  listFirst = 0;
  rebuildList();
  showView(View::List);
}

void AbsActivity::rebuildList() {
  listBooks.clear();
  switch (listKind) {
    case ListKind::Az:
      listBooks.assign(index.byTitle().begin(), index.byTitle().end());
      break;
    case ListKind::Author:
      for (const AbsIndex::Author& a : index.authors()) {
        if (index.text(a.name) != listKey) continue;
        listBooks.assign(index.authorBooks().begin() + a.first, index.authorBooks().begin() + a.first + a.count);
        break;
      }
      break;
    case ListKind::Search:
      listBooks = index.search(listKey);
      break;
  }
  if (listFirst >= static_cast<int>(listBooks.size())) listFirst = 0;
}

void AbsActivity::resumeIfAsked() {
  if (resumeId.empty() || busy) return;
  const std::string id = resumeId;
  resumeId.clear();
  autoPlay = true;
  openBook(id, View::Home);
}

void AbsActivity::playIfAsked() {
  if (!autoPlay) return;
  autoPlay = false;
  refreshLocalState();
  run(localComplete ? Action::PlayLocal : Action::Stream, 0);
}

void AbsActivity::openBook(const std::string& itemId, const View from) {
  detailFrom = from;
  if (!AbsWifi::connected()) {
    // Offline: a downloaded book opens from what was saved with it.
    AbsLocal::BookMeta meta;
    if (AbsLocal::find(itemId, meta) && meta.complete) {
      detail = Abs::ItemDetail{};
      detail.id = meta.itemId;
      detail.title = meta.title;
      detail.author = meta.author;
      detail.durationMs = meta.durationMs;
      detail.tracks = meta.tracks;
      detail.chapters = meta.chapters;
      serverProgress = Abs::Progress{};
      error.clear();
      refreshLocalState();
      showView(View::Detail);
      playIfAsked();
      return;
    }
  }
  loadDetail(itemId);
}

void AbsActivity::onExit() {
  AbsCovers::cancel();
  if (AbsDownloader::status().active) AbsDownloader::pause();
  // The workers write into this activity: let them finish (a request times out within ~20 s).
  for (int i = 0; i < 500 && (busy || refreshing); ++i) vTaskDelay(pdMS_TO_TICKS(50));
  deleteRefreshTask();
  ActivityWithSubactivity::onExit();
  // A worker still running keeps its Wi-Fi until it returns; release() only drops our share.
  if (wifiHeld.exchange(false)) AbsWifi::release();
}

bool AbsActivity::preventAutoSleep() {
  return ActivityWithSubactivity::preventAutoSleep() || busy || AbsDownloader::status().active;
}

void AbsActivity::showView(const View next) {
  view = next;
  if (view == View::List) downloaded = AbsLocal::listDownloaded();
  updateRequired = true;
}

void AbsActivity::startWork(const char* text, std::function<void(std::string& error)> work, const View nextView,
                            const Network network) {
  if (busy) return;
  if (network == Network::Required && !AbsWifi::connected()) {
    // Wi-Fi is down (or dropped since): ask for it, then do this.
    if (wifiHeld.exchange(false)) AbsWifi::release();
    requestWifi([this, label = std::string(text), work, nextView] { startWork(label.c_str(), work, nextView); });
    return;
  }
  wifiNeeded = false;
  busy = true;
  busyText = text;
  error.clear();
  updateRequired = true;
  auto* job = new WorkerJob();
  job->body = [this, work, nextView, network] {
    std::string failure;
    if (!wifiHeld) {
      if (AbsWifi::acquire(failure)) {
        wifiHeld = true;
      } else if (network == Network::BestEffort) {
        failure.clear();  // do the local part anyway
      }
    }
    if (failure.empty()) work(failure);
    xSemaphoreTake(dataLock, portMAX_DELAY);
    error = failure;
    if (failure.empty()) view = nextView;
    xSemaphoreGive(dataLock);
    resultReady = true;
    busy = false;
  };
  if (xTaskCreate(workerMain, "abs_ui", WORKER_STACK, job, 1, nullptr) != pdPASS) {
    delete job;
    busy = false;
    error = "Out of memory";
  }
}

void AbsActivity::requestWifi(std::function<void()> then) {
  pending = std::move(then);
  enterNewActivity(new WifiSelectionActivity(renderer, mappedInput, [this](const bool connected) {
    exitActivity();
    AbsWifi::prepare();  // the picker may have saved a network
    if (connected) {
      AbsWifi::adopt();
      offline = false;
      // The screen keeps the connection until it is left (onExit releases it). Otherwise work
      // that holds it only while it runs (a refresh) would turn Wi-Fi off when done, and the
      // next book opened would ask for Wi-Fi again.
      std::string ignored;
      if (!wifiHeld && AbsWifi::acquire(ignored)) wifiHeld = true;
      if (pending) {
        const std::function<void()> next = std::move(pending);
        pending = nullptr;
        next();
      }
    } else {
      AbsWifi::offIfUnused();
      wifiNeeded = true;
      xSemaphoreTake(dataLock, portMAX_DELAY);
      error = "Audiobookshelf needs Wi-Fi.";
      xSemaphoreGive(dataLock);
    }
    updateRequired = true;
  }));
}

void AbsActivity::loadLibraries() {
  startWork("Loading libraries...", [this](std::string& failure) { Abs::libraries(libraries, failure); }, View::More);
}

void AbsActivity::loadHome() {
  startWork(
      "Loading...", [this](std::string& failure) { Abs::shelves(library.id, SHELF_BOOKS, shelves, failure); },
      View::Home);
}

void AbsActivity::loadDetail(const std::string& itemId) {
  startWork(
      "Loading book...",
      [this, itemId](std::string& failure) {
        if (Abs::item(itemId, detail, failure)) Abs::progress(itemId, serverProgress, failure);
      },
      View::Detail);
}

void AbsActivity::refreshLocalState() {
  AbsLocal::BookMeta meta;
  localCopy = AbsLocal::find(detail.id, meta);
  localComplete = localCopy && meta.complete;
}

void AbsActivity::editField(const char* title, std::string& field, const bool secret) {
  enterNewActivity(new KeyboardEntryActivity(
      renderer, mappedInput, title, field, 10, 128, secret,
      [this, &field](const std::string& text) {
        field = text;
        exitActivity();
        updateRequired = true;
      },
      [this] {
        exitActivity();
        updateRequired = true;
      }));
}

/** Rows of a paged list view under the header, above the pager buttons. */
int AbsActivity::rowsPerPage(const int rowHeight) const {
  const int pagerY = renderer.getScreenHeight() - BUTTON_HEIGHT - 2 * GAP;
  return std::max(1, (pagerY - contentTop - 30) / rowHeight);  // 30: room for the status line
}

void AbsActivity::loop() {
  if (subActivity) {
    subActivity->loop();
    return;
  }
  if (view == View::Download) AbsDownloader::service();

  if (resultReady.exchange(false)) {
    // The worker finished: views that need SD state get it here, on the main loop.
    if (view == View::Detail) {
      refreshLocalState();
      if (error.empty()) playIfAsked();
    }
    if (view == View::More && error.empty() && !libraries.empty()) {
      AbsLibrary::saveLibraries(libraries);
      if (libraries.size() == 1) {
        library = libraries.front();
        lastLibraryId = library.id;
        loadHome();
      }
    }
    if (view == View::Home && error.empty()) {
      // First visit: the shelves came from the server; the index follows in the background.
      AbsLibrary::saveShelves(library.id, shelves);
      if (index.empty()) AbsLibrary::loadIndex(library.id, index);
      startRefresh(false);
      resumeIfAsked();
    }
    updateRequired = true;
  }
  if (refreshReady.exchange(false)) applyRefresh();

  const auto close = [this] {
    if (busy) return;
    if (wifiNeeded) {
      // Give up on the work that wanted Wi-Fi and go back to the view it started from; on entry
      // (signed in, nothing loaded yet) there is none, so leave.
      wifiNeeded = false;
      pending = nullptr;
      error.clear();
      if (view == View::SignIn && Abs::signedIn()) {
        onClose();
        return;
      }
      updateRequired = true;
      return;
    }
    switch (view) {
      case View::Download:
        AbsDownloader::pause();
        refreshLocalState();
        showView(View::Detail);
        return;
      case View::Detail:
        showView(detailFrom);
        // Continue Listening may have changed while the book was open.
        if (detailFrom == View::Home) startRefresh(false);
        return;
      case View::List:
        showView(listKind == ListKind::Author ? View::Authors : View::Home);
        return;
      case View::Authors:
        showView(View::Home);
        return;
      case View::Letters:
        showView(View::List);
        return;
      case View::More:
        if (!library.id.empty()) {
          showView(View::Home);
          return;
        }
        break;
      default:
        break;
    }
    onClose();
  };
  if (SubPage::closeInput(renderer, mappedInput, close)) return;

  if (!busy && mappedInput.hasTouch()) {
    float nx = 0.0f;
    float ny = 0.0f;
    if (mappedInput.wasTouchTapInScreen(renderer, nx, ny)) {
      const int x = static_cast<int>(nx * renderer.getScreenWidth());
      const int y = static_cast<int>(ny * renderer.getScreenHeight());
      const auto hit = std::find_if(targets.begin(), targets.end(), [x, y](const Target& t) {
        return x >= t.x && x < t.x + t.w && y >= t.y && y < t.y + t.h;
      });
      if (hit != targets.end()) {
        run(hit->action, hit->index);
        return;
      }
    }
  }
  if (!busy && (view == View::List || view == View::Authors)) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Up)) run(Action::PageUp, 0);
    if (mappedInput.wasPressed(MappedInputManager::Button::Down)) run(Action::PageDown, 0);
  }

  const bool coversArrived = AbsCovers::changeCounter() != renderedCovers;
  const bool coverTick = coversArrived && !busy && millis() - lastRenderMs >= COVER_REDRAW_MS;
  const bool downloadTick = view == View::Download && millis() - lastRenderMs >= DOWNLOAD_REDRAW_MS;
  if (updateRequired || downloadTick || coverTick) {
    updateRequired = false;
    render();
  }
}

void AbsActivity::run(const Action action, const int index) {
  switch (action) {
    case Action::EditServer:
      editField("Audiobookshelf server", server, false);
      return;
    case Action::EditUser:
      editField("Username", user, false);
      return;
    case Action::EditPassword:
      editField("Password", password, true);
      return;
    case Action::SignIn: {
      const std::string s = server, u = user, p = password;
      startWork(
          "Signing in...",
          [this, s, u, p](std::string& failure) {
            if (Abs::signIn(s, u, p, failure)) Abs::libraries(libraries, failure);
          },
          View::More);
      password.clear();
      return;
    }
    case Action::WifiSettings:
      enterNewActivity(new WifiSelectionActivity(renderer, mappedInput, [this](const bool connected) {
        exitActivity();
        AbsWifi::prepare();
        if (connected) AbsWifi::adopt();
        updateRequired = true;
      }));
      return;
    case Action::RetryWifi:
      requestWifi(pending);
      return;
    case Action::SignOut:
      // Tells the server when it can; signing out here works offline too. Forgets the cache.
      AbsLibrary::clear();
      this->index.clear();
      shelves.clear();
      libraries.clear();
      library = Abs::Library{};
      lastLibraryId.clear();
      indexFetchedFor.clear();
      startWork("Signing out...", [](std::string&) { Abs::signOut(); }, View::SignIn, Network::BestEffort);
      return;
    case Action::OpenLibrary:
      if (index >= 0 && index < static_cast<int>(libraries.size())) {
        library = libraries[index];
        lastLibraryId = library.id;
        shelves.clear();
        this->index.clear();
        const bool cached = AbsLibrary::loadShelves(library.id, shelves);
        AbsLibrary::loadIndex(library.id, this->index);
        if (cached) {
          showView(View::Home);
          startRefresh(false);
        } else {
          loadHome();
        }
      }
      return;
    case Action::OpenBook:
      if (index >= 0 && index < static_cast<int>(listBooks.size())) {
        openBook(std::string(this->index.text(this->index.book(listBooks[index]).id)), View::List);
      }
      return;
    case Action::OpenShelfBook: {
      const size_t shelf = static_cast<size_t>(index / SHELF_INDEX);
      const size_t row = static_cast<size_t>(index % SHELF_INDEX);
      if (shelf < shelves.size() && row < shelves[shelf].items.size())
        openBook(shelves[shelf].items[row].id, View::Home);
      return;
    }
    case Action::ShowAz:
      openList(ListKind::Az, "");
      return;
    case Action::ShowAuthors:
      authorsFirst = 0;
      showView(View::Authors);
      return;
    case Action::OpenAuthor:
      if (index >= 0 && index < static_cast<int>(this->index.authors().size())) {
        openList(ListKind::Author, std::string(this->index.text(this->index.authors()[index].name)));
      }
      return;
    case Action::ShowSearch:
      enterNewActivity(new KeyboardEntryActivity(
          renderer, mappedInput, "Search titles and authors", listKind == ListKind::Search ? listKey : "", 10, 64,
          false,
          [this](const std::string& text) {
            exitActivity();
            if (text.empty()) {
              updateRequired = true;
            } else {
              openList(ListKind::Search, text);
            }
          },
          [this] {
            exitActivity();
            updateRequired = true;
          }));
      return;
    case Action::ShowLetters:
      showView(View::Letters);
      return;
    case Action::PickLetter:
      listFirst = static_cast<int>(this->index.firstAt(static_cast<char>(index)));
      showView(View::List);
      return;
    case Action::ShowMore:
      showView(View::More);
      return;
    case Action::Refresh:
      if (AbsWifi::connected()) {
        startRefresh(true);
      } else {
        requestWifi([this] { startRefresh(true); });
      }
      return;
    case Action::PageUp:
    case Action::PageDown: {
      const bool down = action == Action::PageDown;
      if (view == View::Authors) {
        const int rows = rowsPerPage(ROW_HEIGHT);
        const int count = static_cast<int>(this->index.authors().size());
        authorsFirst = std::max(0, std::min(authorsFirst + (down ? rows : -rows), std::max(0, count - 1)));
      } else {
        const int rows = rowsPerPage(BOOK_ROW_HEIGHT);
        const int count = static_cast<int>(listBooks.size());
        listFirst = std::max(0, std::min(listFirst + (down ? rows : -rows), std::max(0, count - 1)));
      }
      updateRequired = true;
      return;
    }
    case Action::Stream:
      if (AudioPlayer::openRemote(detail)) onPlay();
      return;
    case Action::PlayLocal: {
      AbsLocal::BookMeta meta;
      if (AbsLocal::find(detail.id, meta) && meta.complete) {
        AudioPlayer::Book book{meta.folder, meta.title, true, meta.itemId};
        if (AudioPlayer::open(book)) onPlay();
      }
      return;
    }
    case Action::Download: {
      std::string failure;
      if (AbsDownloader::start(detail, failure)) {
        showView(View::Download);
      } else {
        error = failure;
        updateRequired = true;
      }
      return;
    }
    case Action::DeleteLocal: {
      AbsLocal::BookMeta meta;
      if (AbsLocal::find(detail.id, meta)) AbsLocal::remove(meta);
      refreshLocalState();
      updateRequired = true;
      return;
    }
    case Action::PauseDownload:
      AbsDownloader::pause();
      refreshLocalState();
      showView(View::Detail);
      return;
  }
}

void AbsActivity::button(const int x, const int y, const int w, const int h, const char* label, const Action action,
                         const int index, const bool filled) {
  if (filled) {
    renderer.rectangle.fill(x, y, w, h, true, true);
  } else {
    renderer.rectangle.render(x, y, w, h, true, true);
  }
  const int font = systemFontId();
  const std::string text = renderer.text.truncate(font, label, w - 16);
  const int textW = renderer.text.getWidth(font, text.c_str());
  renderer.text.render(font, x + (w - textW) / 2, y + (h - renderer.text.getLineHeight(font)) / 2, text.c_str(),
                       !filled);
  targets.push_back({x, y, w, h, action, index});
}

void AbsActivity::drawCover(const std::string& itemId, const int x, const int y, const int size) {
  if (AbsCovers::cached(itemId)) {
    ImageRender::Options options;
    options.cropToFill = true;  // square, whatever the cover's shape
    if (ImageRender::create(renderer, AbsCovers::path(itemId)).render(x, y, size, size, options)) return;
  }
  renderer.rectangle.dotted(x, y, size, size, true);
  missingCovers.push_back(itemId);
}

void AbsActivity::row(const int y, const int height, const Action action, const int index) {
  const int width = renderer.getScreenWidth();
  renderer.line.render(0, y + height - 1, width, y + height - 1, true, LineRender::Style::Dotted);
  targets.push_back({0, y, width, height, action, index});
}

void AbsActivity::render() {
  targets.clear();
  missingCovers.clear();
  renderedCovers = AbsCovers::changeCounter();
  renderer.clearScreen();
  std::string title;
  switch (view) {
    case View::List:
      title = listKind == ListKind::Az ? "A-Z" : (listKind == ListKind::Author ? listKey : "\"" + listKey + "\"");
      break;
    case View::Detail:
      title = "Book";
      break;
    case View::Download:
      title = "Download";
      break;
    case View::Authors:
      title = "Authors";
      break;
    case View::Letters:
      title = "Jump to";
      break;
    case View::Home:
      title = libraries.size() > 1 && !library.name.empty() ? library.name : "Audiobookshelf";
      break;
    default:
      title = "Audiobookshelf";
      break;
  }
  int top = SubPage::header(renderer, title.c_str()) + 16;
  contentTop = top;
  // Where the shown data stands: from the card while offline, or being refreshed.
  const bool browsing = view == View::Home || view == View::List || view == View::Authors || view == View::More;
  if (!busy && browsing && (offline || refreshing)) {
    constexpr int noteFont = MONTSERRAT_10_FONT_ID;
    renderer.text.render(noteFont, MARGIN, top - 8, offline ? "Offline: showing the saved library." : "Updating...");
    top += renderer.text.getLineHeight(noteFont);
  }
  const int font = systemFontId();
  const int width = renderer.getScreenWidth();

  if (busy) {
    renderer.text.render(font, MARGIN, top, busyText.c_str());
  } else {
    int y = top;
    std::string shownError;
    xSemaphoreTake(dataLock, portMAX_DELAY);
    shownError = error;
    xSemaphoreGive(dataLock);
    if (!shownError.empty()) {
      // Word-wrapped: connection errors explain what to change, which takes more than a line.
      constexpr int smallFont = MONTSERRAT_10_FONT_ID;
      const int maxWidth = width - 2 * MARGIN;
      std::string line;
      size_t at = 0;
      const auto flush = [&] {
        renderer.text.render(smallFont, MARGIN, y, renderer.text.truncate(smallFont, line.c_str(), maxWidth).c_str(),
                             true, EpdFontFamily::BOLD);
        y += renderer.text.getLineHeight(smallFont);
        line.clear();
      };
      while (at < shownError.size()) {
        const size_t space = shownError.find(' ', at);
        const std::string word = shownError.substr(at, space == std::string::npos ? std::string::npos : space - at);
        at = space == std::string::npos ? shownError.size() : space + 1;
        const std::string candidate = line.empty() ? word : line + " " + word;
        if (!line.empty() && renderer.text.getWidth(smallFont, candidate.c_str()) > maxWidth) {
          flush();
          line = word;
        } else {
          line = candidate;
        }
      }
      if (!line.empty()) flush();
      y += GAP;
    }
    if (wifiNeeded) {
      // The picker was cancelled: the work it was for is still waiting.
      button(MARGIN, y, width - 2 * MARGIN, BUTTON_HEIGHT, "Connect to Wi-Fi", Action::RetryWifi, 0, true);
      renderer.displayBuffer();
      lastRenderMs = millis();
      return;
    }
    switch (view) {
      case View::SignIn:
        renderSignIn(y);
        break;
      case View::More:
        renderMore(y);
        break;
      case View::List:
        renderList(y);
        break;
      case View::Authors:
        renderAuthors(y);
        break;
      case View::Letters:
        renderLetters(y);
        break;
      case View::Home:
        renderHome(y);
        break;
      case View::Detail:
        renderDetail(y);
        break;
      case View::Download:
        renderDownload(y);
        break;
    }
  }
  renderer.displayBuffer();
  lastRenderMs = millis();
  AbsCovers::request(missingCovers);
}

void AbsActivity::renderSignIn(int y) {
  const int width = renderer.getScreenWidth();
  const int inner = width - 2 * MARGIN;
  const int font = systemFontId();
  constexpr int labelFont = MONTSERRAT_8_FONT_ID;
  struct Field {
    const char* label;
    std::string value;
    Action action;
  };
  const Field fields[] = {{"SERVER", server.empty() ? "https://..." : server, Action::EditServer},
                          {"USERNAME", user, Action::EditUser},
                          {"PASSWORD", std::string(password.size(), '*'), Action::EditPassword}};
  for (const Field& field : fields) {
    renderer.text.render(labelFont, MARGIN, y + 6, field.label, true, EpdFontFamily::BOLD);
    renderer.text.render(font, MARGIN, y + 28, renderer.text.truncate(font, field.value.c_str(), inner).c_str());
    row(y, ROW_HEIGHT + 10, field.action, 0);
    y += ROW_HEIGHT + 10;
  }
  y += GAP;
  button(MARGIN, y, inner, BUTTON_HEIGHT, "Sign in", Action::SignIn, 0, true);
  y += BUTTON_HEIGHT + GAP;
  if (!AbsWifi::hasSavedNetworks()) {
    button(MARGIN, y, inner, BUTTON_HEIGHT, "Set up Wi-Fi", Action::WifiSettings);
  }
}

void AbsActivity::renderMore(int y) {
  const int width = renderer.getScreenWidth();
  const int inner = width - 2 * MARGIN;
  const int font = systemFontId();
  constexpr int smallFont = MONTSERRAT_10_FONT_ID;
  if (!refreshError.empty() && !refreshing) {
    const std::string note = "Not updated: " + refreshError;
    renderer.text.render(smallFont, MARGIN, y, renderer.text.truncate(smallFont, note.c_str(), inner).c_str());
    y += renderer.text.getLineHeight(smallFont) + GAP;
  }
  if (!index.empty()) {
    const std::string count = std::to_string(index.size()) + " books in the saved library.";
    renderer.text.render(smallFont, MARGIN, y, count.c_str());
    y += renderer.text.getLineHeight(smallFont) + GAP;
  }
  if (libraries.empty()) {
    renderer.text.render(font, MARGIN, y, "No audiobook libraries on this server.");
    y += renderer.text.getLineHeight(font) + 2 * GAP;
  }
  if (libraries.size() > 1 || library.id.empty()) {
    for (size_t i = 0; i < libraries.size(); ++i) {
      const bool current = libraries[i].id == library.id;
      const std::string label = (current ? "> " : "") + libraries[i].name;
      renderer.text.render(font, MARGIN, y + (ROW_HEIGHT - renderer.text.getLineHeight(font)) / 2,
                           renderer.text.truncate(font, label.c_str(), inner).c_str(), true,
                           current ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
      row(y, ROW_HEIGHT, Action::OpenLibrary, static_cast<int>(i));
      y += ROW_HEIGHT;
    }
    y += 2 * GAP;
  }
  const int half = (inner - GAP) / 2;
  button(MARGIN, y, half, BUTTON_HEIGHT, refreshing ? "Updating..." : "Refresh", Action::Refresh);
  button(MARGIN + half + GAP, y, inner - half - GAP, BUTTON_HEIGHT, "Sign out", Action::SignOut);
}

void AbsActivity::pager(const int y, const bool previous, const bool next, const bool jump) {
  const int inner = renderer.getScreenWidth() - 2 * MARGIN;
  const int third = (inner - 2 * GAP) / 3;
  if (previous) button(MARGIN, y, third, BUTTON_HEIGHT, "Previous", Action::PageUp);
  if (jump) button(MARGIN + third + GAP, y, third, BUTTON_HEIGHT, "Jump", Action::ShowLetters);
  if (next) button(MARGIN + 2 * (third + GAP), y, third, BUTTON_HEIGHT, "Next", Action::PageDown);
}

void AbsActivity::renderList(int y) {
  const int width = renderer.getScreenWidth();
  const int inner = width - 2 * MARGIN;
  const int font = systemFontId();
  constexpr int smallFont = MONTSERRAT_10_FONT_ID;
  if (listBooks.empty()) {
    const char* message = !index.empty() ? "No books."
                          : refreshing   ? "Loading the library..."
                          : offline      ? "The library is not saved yet: connect once to load it."
                                         : "The library is not loaded yet.";
    renderer.text.render(font, MARGIN, y, message);
  }
  const int rows = rowsPerPage(BOOK_ROW_HEIGHT);
  const int count = static_cast<int>(listBooks.size());
  for (int i = listFirst; i < count && i < listFirst + rows; ++i) {
    const AbsIndex::Book& b = index.book(listBooks[i]);
    const std::string id(index.text(b.id));
    const int cover = BOOK_ROW_HEIGHT - 2 * ROW_COVER_INSET;
    drawCover(id, MARGIN, y + ROW_COVER_INSET, cover);
    const int textX = MARGIN + cover + GAP;
    const int textW = inner - cover - GAP;
    renderer.text.render(font, textX, y + 10,
                         renderer.text.truncate(font, std::string(index.text(b.title)).c_str(), textW).c_str(), true,
                         EpdFontFamily::BOLD);
    // Progress first, so a long author name truncates instead of it.
    std::string info;
    if (b.finished) {
      info = "Finished  -  ";
    } else if (b.progress > 0) {
      info = std::to_string(b.progress / 10) + "%  -  ";
    }
    const std::string_view author = index.text(b.author);
    if (!author.empty()) info += std::string(author) + "  -  ";
    info += formatDuration(b.durationSec);
    const auto local =
        std::find_if(downloaded.begin(), downloaded.end(), [&id](const auto& entry) { return entry.first == id; });
    if (local != downloaded.end()) info += local->second ? "  -  on device" : "  -  partly downloaded";
    renderer.text.render(smallFont, textX, y + 16 + renderer.text.getLineHeight(font),
                         renderer.text.truncate(smallFont, info.c_str(), textW).c_str());
    row(y, BOOK_ROW_HEIGHT, Action::OpenBook, i);
    y += BOOK_ROW_HEIGHT;
  }
  pager(renderer.getScreenHeight() - BUTTON_HEIGHT - 2 * GAP, listFirst > 0, listFirst + rows < count,
        listKind == ListKind::Az && count > rows);
}

void AbsActivity::renderAuthors(int y) {
  const int width = renderer.getScreenWidth();
  const int inner = width - 2 * MARGIN;
  const int font = systemFontId();
  constexpr int smallFont = MONTSERRAT_10_FONT_ID;
  const auto& authors = index.authors();
  if (authors.empty()) {
    renderer.text.render(font, MARGIN, y, refreshing ? "Loading the library..." : "No authors.");
  }
  const int rows = rowsPerPage(ROW_HEIGHT);
  const int count = static_cast<int>(authors.size());
  for (int i = authorsFirst; i < count && i < authorsFirst + rows; ++i) {
    const std::string books = std::to_string(authors[i].count) + (authors[i].count == 1 ? " book" : " books");
    const int booksW = renderer.text.getWidth(smallFont, books.c_str());
    renderer.text.render(
        font, MARGIN, y + (ROW_HEIGHT - renderer.text.getLineHeight(font)) / 2,
        renderer.text.truncate(font, std::string(index.text(authors[i].name)).c_str(), inner - booksW - GAP).c_str());
    renderer.text.render(smallFont, MARGIN + inner - booksW,
                         y + (ROW_HEIGHT - renderer.text.getLineHeight(smallFont)) / 2, books.c_str());
    row(y, ROW_HEIGHT, Action::OpenAuthor, i);
    y += ROW_HEIGHT;
  }
  pager(renderer.getScreenHeight() - BUTTON_HEIGHT - 2 * GAP, authorsFirst > 0, authorsFirst + rows < count, false);
}

void AbsActivity::renderLetters(int y) {
  // # (digits and the rest) and A-Z, seven to a row; a letter without titles goes to the next.
  const int inner = renderer.getScreenWidth() - 2 * MARGIN;
  constexpr int COLUMNS = 7;
  const int cell = (inner - (COLUMNS - 1) * GAP) / COLUMNS;
  const char letters[] = "#ABCDEFGHIJKLMNOPQRSTUVWXYZ";
  for (int i = 0; letters[i]; ++i) {
    const char label[2] = {letters[i], '\0'};
    button(MARGIN + (i % COLUMNS) * (cell + GAP), y + (i / COLUMNS) * (BUTTON_HEIGHT + GAP), cell, BUTTON_HEIGHT, label,
           Action::PickLetter, letters[i]);
  }
}

void AbsActivity::renderHome(int y) {
  const int width = renderer.getScreenWidth();
  const int inner = width - 2 * MARGIN;
  const int font = systemFontId();
  constexpr int labelFont = MONTSERRAT_10_FONT_ID;
  constexpr int smallFont = MONTSERRAT_10_FONT_ID;
  const int buttonsY = renderer.getScreenHeight() - BUTTON_HEIGHT - 2 * GAP;

  if (shelves.empty()) {
    renderer.text.render(font, MARGIN, y, "Nothing in progress or recently added.");
  }
  for (size_t s = 0; s < shelves.size(); ++s) {
    const Abs::Shelf& shelf = shelves[s];
    const int labelH = renderer.text.getLineHeight(labelFont) + 8;
    // Each shelf shows what fits; one that would not get a single row is left for "All books".
    if (y + labelH + SHELF_ROW_HEIGHT > buttonsY - GAP) break;
    std::string label = shelf.label;
    std::transform(label.begin(), label.end(), label.begin(), [](unsigned char c) { return std::toupper(c); });
    renderer.text.render(labelFont, MARGIN, y + 4, label.c_str(), true, EpdFontFamily::BOLD);
    y += labelH;
    for (size_t i = 0; i < shelf.items.size() && y + SHELF_ROW_HEIGHT <= buttonsY - GAP; ++i) {
      const Abs::ItemSummary& b = shelf.items[i];
      const int cover = SHELF_ROW_HEIGHT - 2 * ROW_COVER_INSET;
      drawCover(b.id, MARGIN, y + ROW_COVER_INSET, cover);
      const int textX = MARGIN + cover + GAP;
      const int textW = inner - cover - GAP;
      renderer.text.render(font, textX, y + 8, renderer.text.truncate(font, b.title.c_str(), textW).c_str(), true,
                           EpdFontFamily::BOLD);
      // Progress first, so a long author name truncates instead of it.
      std::string info;
      if (b.finished) {
        info = "Finished  -  ";
      } else if (b.progress > 0.0f) {
        info = std::to_string(static_cast<int>(b.progress * 100)) + "%  -  ";
      }
      if (!b.author.empty()) info += b.author + "  -  ";
      info += formatDuration(b.durationSec);
      const int infoY = y + 12 + renderer.text.getLineHeight(font);
      renderer.text.render(smallFont, textX, infoY, renderer.text.truncate(smallFont, info.c_str(), textW).c_str());
      row(y, SHELF_ROW_HEIGHT, Action::OpenShelfBook, static_cast<int>(s) * SHELF_INDEX + static_cast<int>(i));
      y += SHELF_ROW_HEIGHT;
    }
    y += GAP;
  }

  // Browse the cached library; More holds libraries, refresh and sign out.
  const int quarter = (inner - 3 * GAP) / 4;
  const struct {
    const char* label;
    Action action;
  } buttons[] = {{"A-Z", Action::ShowAz},
                 {"Authors", Action::ShowAuthors},
                 {"Search", Action::ShowSearch},
                 {"More", Action::ShowMore}};
  for (int i = 0; i < 4; ++i) {
    button(MARGIN + i * (quarter + GAP), buttonsY, quarter, BUTTON_HEIGHT, buttons[i].label, buttons[i].action, 0,
           i == 0);
  }
}

void AbsActivity::renderDetail(int y) {
  const int width = renderer.getScreenWidth();
  const int inner = width - 2 * MARGIN;
  const int font = systemFontId();
  constexpr int titleFont = MONTSERRAT_18_FONT_ID;
  constexpr int smallFont = MONTSERRAT_10_FONT_ID;

  // The cover on the left, the details beside it; the buttons go under whichever is taller.
  const int coverTop = y;
  drawCover(detail.id, MARGIN, coverTop, DETAIL_COVER);
  const int textX = MARGIN + DETAIL_COVER + GAP;
  const int textW = inner - DETAIL_COVER - GAP;
  // The title may take two lines here: whole words on the first, the rest truncated on the second.
  std::string first;
  std::string rest = detail.title;
  for (size_t at = 0; at <= detail.title.size();) {
    const size_t space = detail.title.find(' ', at);
    const size_t end = space == std::string::npos ? detail.title.size() : space;
    const std::string candidate = detail.title.substr(0, end);
    if (renderer.text.getWidth(titleFont, candidate.c_str()) > textW) break;
    first = candidate;
    rest = end < detail.title.size() ? detail.title.substr(end + 1) : "";
    if (space == std::string::npos) break;
    at = space + 1;
  }
  if (first.empty()) {
    first = renderer.text.truncate(titleFont, detail.title.c_str(), textW);  // one long word
    rest.clear();
  }
  renderer.text.render(titleFont, textX, y, first.c_str(), true, EpdFontFamily::BOLD);
  y += renderer.text.getLineHeight(titleFont);
  if (!rest.empty()) {
    renderer.text.render(titleFont, textX, y, renderer.text.truncate(titleFont, rest.c_str(), textW).c_str(), true,
                         EpdFontFamily::BOLD);
    y += renderer.text.getLineHeight(titleFont);
  }
  y += GAP;
  if (!detail.author.empty()) {
    renderer.text.render(font, textX, y, renderer.text.truncate(font, detail.author.c_str(), textW).c_str());
    y += renderer.text.getLineHeight(font) + GAP;
  }
  const uint64_t size = std::accumulate(detail.tracks.begin(), detail.tracks.end(), uint64_t{0},
                                        [](const uint64_t sum, const Abs::Track& t) { return sum + t.sizeBytes; });
  char info[128];
  snprintf(info, sizeof(info), "%s  -  %u %s  -  %s", formatDuration(detail.durationMs / 1000).c_str(),
           static_cast<unsigned>(detail.tracks.size()), detail.tracks.size() == 1 ? "file" : "files",
           formatMb(size).c_str());
  renderer.text.render(smallFont, textX, y, renderer.text.truncate(smallFont, info, textW).c_str());
  y += renderer.text.getLineHeight(smallFont) + 4;
  if (serverProgress.exists) {
    snprintf(info, sizeof(info), "Listened: %s of %s%s", formatDuration(serverProgress.currentMs / 1000).c_str(),
             formatDuration(detail.durationMs / 1000).c_str(), serverProgress.finished ? " (finished)" : "");
    renderer.text.render(smallFont, textX, y, renderer.text.truncate(smallFont, info, textW).c_str());
    y += renderer.text.getLineHeight(smallFont) + 4;
  }
  y = std::max(y, coverTop + DETAIL_COVER) + 2 * GAP;

  button(MARGIN, y, inner, BUTTON_HEIGHT, "Stream over Wi-Fi", Action::Stream, 0, !localComplete);
  y += BUTTON_HEIGHT + GAP;
  if (localComplete) {
    button(MARGIN, y, inner, BUTTON_HEIGHT, "Play from device", Action::PlayLocal, 0, true);
    y += BUTTON_HEIGHT + GAP;
    button(MARGIN, y, inner, BUTTON_HEIGHT, "Delete from device", Action::DeleteLocal);
  } else if (localCopy) {
    button(MARGIN, y, inner, BUTTON_HEIGHT, "Resume download", Action::Download);
    y += BUTTON_HEIGHT + GAP;
    button(MARGIN, y, inner, BUTTON_HEIGHT, "Delete partial download", Action::DeleteLocal);
  } else {
    button(MARGIN, y, inner, BUTTON_HEIGHT, "Download to device", Action::Download);
  }
}

void AbsActivity::renderDownload(int y) {
  const AbsDownloader::Status s = AbsDownloader::status();
  const int width = renderer.getScreenWidth();
  const int inner = width - 2 * MARGIN;
  const int font = systemFontId();
  constexpr int smallFont = MONTSERRAT_10_FONT_ID;

  renderer.text.render(font, MARGIN, y, renderer.text.truncate(font, s.title.c_str(), inner).c_str(), true,
                       EpdFontFamily::BOLD);
  y += renderer.text.getLineHeight(font) + 2 * GAP;
  renderer.rectangle.render(MARGIN, y, inner, 18, true);
  if (s.totalBytes) {
    const int filled = static_cast<int>(static_cast<uint64_t>(inner - 4) * s.doneBytes / s.totalBytes);
    if (filled > 0) renderer.rectangle.fill(MARGIN + 2, y + 2, filled, 14, true);
  }
  y += 18 + GAP;
  char line[128];
  snprintf(line, sizeof(line), "%s of %s  -  file %d of %d", formatMb(s.doneBytes).c_str(),
           formatMb(s.totalBytes).c_str(), s.track + 1, s.trackCount);
  renderer.text.render(smallFont, MARGIN, y, line);
  y += renderer.text.getLineHeight(smallFont) + 4;
  if (s.active && s.bytesPerSecond) {
    const uint64_t left = s.totalBytes > s.doneBytes ? s.totalBytes - s.doneBytes : 0;
    snprintf(line, sizeof(line), "%.0f KB/s  -  about %s left", s.bytesPerSecond / 1024.0,
             formatDuration(static_cast<uint32_t>(left / s.bytesPerSecond)).c_str());
    renderer.text.render(smallFont, MARGIN, y, line);
  }
  y += renderer.text.getLineHeight(smallFont) + 3 * GAP;

  if (s.done) {
    renderer.text.render(font, MARGIN, y, "Downloaded.");
    y += renderer.text.getLineHeight(font) + 2 * GAP;
    button(MARGIN, y, inner, BUTTON_HEIGHT, "Play from device", Action::PlayLocal, 0, true);
  } else if (s.failed) {
    renderer.text.render(smallFont, MARGIN, y, renderer.text.truncate(smallFont, s.error.c_str(), inner).c_str(), true,
                         EpdFontFamily::BOLD);
    y += renderer.text.getLineHeight(smallFont) + 2 * GAP;
    button(MARGIN, y, inner, BUTTON_HEIGHT, "Retry", Action::Download, 0, true);
  } else {
    renderer.text.render(smallFont, MARGIN, y, "Keep this screen open: the download pauses when you leave it.");
    y += renderer.text.getLineHeight(smallFont) + 2 * GAP;
    button(MARGIN, y, inner, BUTTON_HEIGHT, "Pause", Action::PauseDownload);
  }
}

#endif  // INX_HAS_AUDIO_MODULE
