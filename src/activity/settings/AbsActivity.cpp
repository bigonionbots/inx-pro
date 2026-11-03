/**
 * @file AbsActivity.cpp
 * @brief Definitions for AbsActivity.
 */

#include "AbsActivity.h"

#if INX_HAS_AUDIO_MODULE

#include <GfxRenderer.h>

#include <algorithm>
#include <cstdio>
#include <memory>
#include <numeric>

#include "abs/AbsDownloader.h"
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
constexpr unsigned long DOWNLOAD_REDRAW_MS = 3000;
constexpr uint32_t WORKER_STACK = 12288;  // TLS

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
    loadLibraries();
  } else {
    showView(View::SignIn);
  }
}

void AbsActivity::onExit() {
  if (AbsDownloader::status().active) AbsDownloader::pause();
  // The worker writes into this activity: let it finish (a request times out within ~20 s).
  for (int i = 0; i < 500 && busy; ++i) vTaskDelay(pdMS_TO_TICKS(50));
  ActivityWithSubactivity::onExit();
  // A worker still running keeps its Wi-Fi until it returns; release() only drops our share.
  if (wifiHeld.exchange(false)) AbsWifi::release();
}

bool AbsActivity::preventAutoSleep() {
  return ActivityWithSubactivity::preventAutoSleep() || busy || AbsDownloader::status().active;
}

void AbsActivity::showView(const View next) {
  view = next;
  if (view == View::Books) downloaded = AbsLocal::listDownloaded();
  updateRequired = true;
}

void AbsActivity::startWork(const char* text, std::function<void(std::string& error)> work, const View nextView) {
  if (busy) return;
  busy = true;
  busyText = text;
  error.clear();
  updateRequired = true;
  auto* job = new WorkerJob();
  job->body = [this, work, nextView] {
    std::string failure;
    if (!wifiHeld) {
      if (AbsWifi::acquire(failure)) {
        wifiHeld = true;
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

void AbsActivity::loadLibraries() {
  startWork("Loading libraries...", [this](std::string& failure) { Abs::libraries(libraries, failure); },
            View::Libraries);
}

void AbsActivity::loadBooks() {
  startWork(
      "Loading books...",
      [this](std::string& failure) { Abs::items(library.id, page, booksPerPage(), books, total, failure); },
      View::Books);
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

int AbsActivity::booksPerPage() const {
  return std::max(1, (renderer.getScreenHeight() - 300) / BOOK_ROW_HEIGHT);
}

void AbsActivity::loop() {
  if (subActivity) {
    subActivity->loop();
    return;
  }
  if (view == View::Download) AbsDownloader::service();

  if (resultReady.exchange(false)) {
    // The worker finished: views that need SD state get it here, on the main loop.
    if (view == View::Detail) refreshLocalState();
    if (view == View::Libraries && error.empty() && libraries.size() == 1) {
      library = libraries.front();
      page = 0;
      loadBooks();
    }
    if (view == View::Books) downloaded = AbsLocal::listDownloaded();
    updateRequired = true;
  }

  const auto close = [this] {
    if (busy) return;
    switch (view) {
      case View::Download:
        AbsDownloader::pause();
        refreshLocalState();
        showView(View::Detail);
        return;
      case View::Detail:
        showView(View::Books);
        return;
      case View::Books:
        if (libraries.size() > 1) {
          showView(View::Libraries);
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
  if (!busy && view == View::Books) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Up)) run(Action::PageUp, 0);
    if (mappedInput.wasPressed(MappedInputManager::Button::Down)) run(Action::PageDown, 0);
  }

  const bool downloadTick = view == View::Download && millis() - lastRenderMs >= DOWNLOAD_REDRAW_MS;
  if (updateRequired || downloadTick) {
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
          View::Libraries);
      password.clear();
      return;
    }
    case Action::WifiSettings:
      enterNewActivity(new WifiSelectionActivity(renderer, mappedInput, [this](bool) {
        exitActivity();
        AbsWifi::prepare();
        updateRequired = true;
      }));
      return;
    case Action::SignOut:
      startWork("Signing out...", [](std::string&) { Abs::signOut(); }, View::SignIn);
      return;
    case Action::OpenLibrary:
      if (index >= 0 && index < static_cast<int>(libraries.size())) {
        library = libraries[index];
        page = 0;
        loadBooks();
      }
      return;
    case Action::OpenBook:
      if (index >= 0 && index < static_cast<int>(books.size())) loadDetail(books[index].id);
      return;
    case Action::PageUp:
      if (page > 0) {
        --page;
        loadBooks();
      }
      return;
    case Action::PageDown:
      if ((page + 1) * booksPerPage() < total) {
        ++page;
        loadBooks();
      }
      return;
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

void AbsActivity::row(const int y, const int height, const Action action, const int index) {
  const int width = renderer.getScreenWidth();
  renderer.line.render(0, y + height - 1, width, y + height - 1, true, LineRender::Style::Dotted);
  targets.push_back({0, y, width, height, action, index});
}

void AbsActivity::render() {
  targets.clear();
  renderer.clearScreen();
  const char* titles[] = {"Audiobookshelf", "Libraries", "Books", "Book", "Download"};
  const char* title = view == View::Books && !library.name.empty() ? library.name.c_str()
                                                                    : titles[static_cast<int>(view)];
  const int top = SubPage::header(renderer, title) + 16;
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
      constexpr int smallFont = MONTSERRAT_10_FONT_ID;
      renderer.text.render(smallFont, MARGIN, y,
                           renderer.text.truncate(smallFont, shownError.c_str(), width - 2 * MARGIN).c_str(), true,
                           EpdFontFamily::BOLD);
      y += renderer.text.getLineHeight(smallFont) + GAP;
    }
    switch (view) {
      case View::SignIn:
        renderSignIn(y);
        break;
      case View::Libraries:
        renderLibraries(y);
        break;
      case View::Books:
        renderBooks(y);
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

void AbsActivity::renderLibraries(int y) {
  const int width = renderer.getScreenWidth();
  const int font = systemFontId();
  if (libraries.empty()) {
    renderer.text.render(font, MARGIN, y, "No audiobook libraries on this server.");
    y += renderer.text.getLineHeight(font) + 2 * GAP;
  }
  for (size_t i = 0; i < libraries.size(); ++i) {
    renderer.text.render(font, MARGIN, y + (ROW_HEIGHT - renderer.text.getLineHeight(font)) / 2,
                         renderer.text.truncate(font, libraries[i].name.c_str(), width - 2 * MARGIN).c_str());
    row(y, ROW_HEIGHT, Action::OpenLibrary, static_cast<int>(i));
    y += ROW_HEIGHT;
  }
  y += 2 * GAP;
  button(MARGIN, y, width - 2 * MARGIN, BUTTON_HEIGHT, "Sign out", Action::SignOut);
}

void AbsActivity::renderBooks(int y) {
  const int width = renderer.getScreenWidth();
  const int inner = width - 2 * MARGIN;
  const int font = systemFontId();
  constexpr int smallFont = MONTSERRAT_10_FONT_ID;
  if (books.empty()) renderer.text.render(font, MARGIN, y, "No books.");
  for (size_t i = 0; i < books.size(); ++i) {
    const Abs::ItemSummary& b = books[i];
    renderer.text.render(font, MARGIN, y + 10, renderer.text.truncate(font, b.title.c_str(), inner).c_str(), true,
                         EpdFontFamily::BOLD);
    std::string info = b.author.empty() ? "" : b.author + "  -  ";
    info += formatDuration(b.durationSec);
    if (b.finished) {
      info += "  -  finished";
    } else if (b.progress > 0.0f) {
      info += "  -  " + std::to_string(static_cast<int>(b.progress * 100)) + "%";
    }
    const auto local = std::find_if(downloaded.begin(), downloaded.end(),
                                    [&b](const auto& entry) { return entry.first == b.id; });
    if (local != downloaded.end()) info += local->second ? "  -  on device" : "  -  partly downloaded";
    renderer.text.render(smallFont, MARGIN, y + 16 + renderer.text.getLineHeight(font),
                         renderer.text.truncate(smallFont, info.c_str(), inner).c_str());
    row(y, BOOK_ROW_HEIGHT, Action::OpenBook, static_cast<int>(i));
    y += BOOK_ROW_HEIGHT;
  }
  const int pagerY = renderer.getScreenHeight() - BUTTON_HEIGHT - 2 * GAP;
  const int third = (inner - 2 * GAP) / 3;
  if (page > 0) button(MARGIN, pagerY, third, BUTTON_HEIGHT, "Previous", Action::PageUp);
  if ((page + 1) * booksPerPage() < total) {
    button(MARGIN + third + GAP, pagerY, third, BUTTON_HEIGHT, "Next", Action::PageDown);
  }
  if (libraries.size() <= 1) button(MARGIN + 2 * (third + GAP), pagerY, third, BUTTON_HEIGHT, "Sign out", Action::SignOut);
}

void AbsActivity::renderDetail(int y) {
  const int width = renderer.getScreenWidth();
  const int inner = width - 2 * MARGIN;
  const int font = systemFontId();
  constexpr int titleFont = MONTSERRAT_18_FONT_ID;
  constexpr int smallFont = MONTSERRAT_10_FONT_ID;

  renderer.text.render(titleFont, MARGIN, y, renderer.text.truncate(titleFont, detail.title.c_str(), inner).c_str(),
                       true, EpdFontFamily::BOLD);
  y += renderer.text.getLineHeight(titleFont) + GAP;
  if (!detail.author.empty()) {
    renderer.text.render(font, MARGIN, y, renderer.text.truncate(font, detail.author.c_str(), inner).c_str());
    y += renderer.text.getLineHeight(font) + GAP;
  }
  const uint64_t size = std::accumulate(detail.tracks.begin(), detail.tracks.end(), uint64_t{0},
                                        [](const uint64_t sum, const Abs::Track& t) { return sum + t.sizeBytes; });
  char info[128];
  snprintf(info, sizeof(info), "%s  -  %u %s  -  %s", formatDuration(detail.durationMs / 1000).c_str(),
           static_cast<unsigned>(detail.tracks.size()), detail.tracks.size() == 1 ? "file" : "files",
           formatMb(size).c_str());
  renderer.text.render(smallFont, MARGIN, y, info);
  y += renderer.text.getLineHeight(smallFont) + 4;
  if (serverProgress.exists) {
    snprintf(info, sizeof(info), "Listened: %s of %s%s", formatDuration(serverProgress.currentMs / 1000).c_str(),
             formatDuration(detail.durationMs / 1000).c_str(), serverProgress.finished ? " (finished)" : "");
    renderer.text.render(smallFont, MARGIN, y, info);
    y += renderer.text.getLineHeight(smallFont) + 4;
  }
  y += 2 * GAP;

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
    renderer.text.render(smallFont, MARGIN, y, renderer.text.truncate(smallFont, s.error.c_str(), inner).c_str(),
                         true, EpdFontFamily::BOLD);
    y += renderer.text.getLineHeight(smallFont) + 2 * GAP;
    button(MARGIN, y, inner, BUTTON_HEIGHT, "Retry", Action::Download, 0, true);
  } else {
    renderer.text.render(smallFont, MARGIN, y, "Keep this screen open: the download pauses when you leave it.");
    y += renderer.text.getLineHeight(smallFont) + 2 * GAP;
    button(MARGIN, y, inner, BUTTON_HEIGHT, "Pause", Action::PauseDownload);
  }
}

#endif  // INX_HAS_AUDIO_MODULE
