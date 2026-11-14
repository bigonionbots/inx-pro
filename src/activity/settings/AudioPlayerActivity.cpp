/**
 * @file AudioPlayerActivity.cpp
 * @brief Definitions for AudioPlayerActivity.
 */

#include "AudioPlayerActivity.h"

#if INX_HAS_AUDIO_MODULE

#include <GfxRenderer.h>
#include <ImageRender.h>
#include <SDCardManager.h>

#include <algorithm>
#include <cstdio>

#include "abs/AbsCovers.h"
#include "abs/AbsQueue.h"
#include "activity/page/SubPage.h"
#include "system/AudioLink.h"
#include "system/Fonts.h"
#include "system/MappedInputManager.h"
#include "system/ScreenComponents.h"
#include "system/UiLayout.h"
#include "util/SdIoMutex.h"

extern void onGoToAudiobookshelf();
extern void onResumeAudiobookshelf(const std::string& itemId);
extern void onOpenAbsDownloads();
extern void onGoToAudioModule();
extern void onGoToListeningStats();

namespace {

constexpr int MARGIN = 20;
constexpr int ROW_HEIGHT = UiLayout::LIST_ITEM_HEIGHT;
constexpr int BUTTON_HEIGHT = 76;
constexpr int GAP = 12;
constexpr int VOLUME_STEP = 10;
constexpr int32_t SKIP_MS = 30000;
/** The Audiobooks page: rows with a cover, and how many recently streamed books it shows. */
constexpr int COVER_ROW_HEIGHT = 84;
constexpr int COVER_INSET = 6;
constexpr size_t RECENT_SHOWN = 3;
constexpr unsigned long POSITION_REDRAW_MS = 10000;
constexpr unsigned long MIN_REDRAW_MS = 1000;

std::string formatTime(const uint32_t ms) {
  const uint32_t total = ms / 1000;
  char text[16];
  if (total >= 3600) {
    snprintf(text, sizeof(text), "%lu:%02lu:%02lu", static_cast<unsigned long>(total / 3600),
             static_cast<unsigned long>(total / 60 % 60), static_cast<unsigned long>(total % 60));
  } else {
    snprintf(text, sizeof(text), "%lu:%02lu", static_cast<unsigned long>(total / 60),
             static_cast<unsigned long>(total % 60));
  }
  return text;
}

const char* outputName(const audiolink::Output output) {
  switch (output) {
    case audiolink::Output::Bluetooth:
      return "Bluetooth";
    case audiolink::Output::I2s:
      return "Wired";
    case audiolink::Output::Null:
      return "Test (silent)";
  }
  return "?";
}

const char* btStateName(const audiolink::BtState state) {
  switch (state) {
    case audiolink::BtState::Off:
      return "off";
    case audiolink::BtState::Idle:
      return "not connected";
    case audiolink::BtState::Scanning:
      return "scanning";
    case audiolink::BtState::Connecting:
      return "connecting";
    case audiolink::BtState::Connected:
      return "connected";
  }
  return "?";
}

const char* moduleStateName(const AudioPlayer::View& v) {
  if (v.waitingForModule) return "Waiting for the audio module...";
  if (v.finished) return "Finished";
  if (v.paused) return "Paused";
  switch (v.moduleState) {
    case audiolink::PlayState::Buffering:
      return v.output == audiolink::Output::Bluetooth && v.btState != audiolink::BtState::Connected
                 ? "Waiting for headphones..."
                 : "Buffering...";
    case audiolink::PlayState::Playing:
    case audiolink::PlayState::Draining:
      return "Playing";
    case audiolink::PlayState::Paused:
      return "Paused";
    default:
      return "";
  }
}

std::string formatAddress(const uint8_t* a) {
  char text[20];
  snprintf(text, sizeof(text), "%02X:%02X:%02X:%02X:%02X:%02X", a[0], a[1], a[2], a[3], a[4], a[5]);
  return text;
}

}  // namespace

void AudioPlayerActivity::onEnter() {
  showView(AudioPlayer::view().open && !startOnBooks ? View::NowPlaying : View::Books);
}

void AudioPlayerActivity::onExit() {
  if (scanning) AudioPlayer::btScan(false);
  scanning = false;
}

void AudioPlayerActivity::showView(const View next) {
  if (view == View::Output && next != View::Output && scanning) {
    AudioPlayer::btScan(false);
    scanning = false;
  }
  view = next;
  if (view == View::Books) {
    books = AudioPlayer::listBooks();
    recent = AudioPlayer::recentAudiobooks();
    if (recent.size() > RECENT_SHOWN) recent.resize(RECENT_SHOWN);
    // Covers once per visit: finding a folder's image reads its directory.
    bookCovers.clear();
    for (const AudioPlayer::Book& b : books) bookCovers.push_back(AudioPlayer::coverFor(b));
    booksFirst = 0;
  }
  if (view == View::Output) AudioPlayer::wakeModule();
  updateRequired = true;
}

void AudioPlayerActivity::loop() {
  const auto close = [this] {
    if (view != View::NowPlaying && AudioPlayer::view().open) {
      showView(View::NowPlaying);
    } else {
      onClose();
    }
  };
  if (SubPage::closeInput(renderer, mappedInput, close)) return;

  if (mappedInput.hasTouch()) {
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
  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    run(Action::PlayPause, 0);
    return;
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Up)) {
    run(view == View::Books      ? Action::BooksPageUp
        : view == View::Chapters ? Action::ChaptersPageUp
                                 : Action::VolumeUp,
        0);
    return;
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Down)) {
    run(view == View::Books      ? Action::BooksPageDown
        : view == View::Chapters ? Action::ChaptersPageDown
                                 : Action::VolumeDown,
        0);
    return;
  }

  const unsigned long now = millis();
  const AudioPlayer::View v = AudioPlayer::view();
  const std::string key = stateKey();
  const bool stateChanged = key != renderedState && now - lastRenderMs >= MIN_REDRAW_MS;
  const bool positionStale =
      view == View::NowPlaying && v.positionMs / 1000 != renderedSecond && now - lastRenderMs >= POSITION_REDRAW_MS;
  if (ScreenComponents::wifiIconStale() && now - lastRenderMs >= MIN_REDRAW_MS) updateRequired = true;
  if (updateRequired || stateChanged || positionStale) {
    updateRequired = false;
    render();
  }
}

std::string AudioPlayerActivity::stateKey() const {
  const AudioPlayer::View v = AudioPlayer::view();
  char key[200];
  // The cover counter: a fetched cover appears without waiting for the next position redraw.
  snprintf(key, sizeof(key), "%d%d%d%d|%u|%u|%u|%u|%d|%d|%zu|%lu|%s|%s", v.open, v.paused, v.finished,
           v.waitingForModule, static_cast<unsigned>(v.moduleState), static_cast<unsigned>(v.btState),
           static_cast<unsigned>(v.output), v.volume, v.track, v.chapter,
           view == View::Output ? AudioLink::btDevices().size() : 0,
           static_cast<unsigned long>(AbsCovers::changeCounter()), v.coverPath.c_str(), v.message.c_str());
  return key;
}

void AudioPlayerActivity::run(const Action action, const int index) {
  const AudioPlayer::View v = AudioPlayer::view();
  switch (action) {
    case Action::PlayPause:
      if (v.open) {
        AudioPlayer::togglePause();
      } else {
        showView(View::Books);
      }
      break;
    case Action::Back30:
      AudioPlayer::skip(-SKIP_MS);
      break;
    case Action::Forward30:
      AudioPlayer::skip(SKIP_MS);
      break;
    case Action::PreviousTrack:
      AudioPlayer::previousTrack();
      break;
    case Action::NextTrack:
      AudioPlayer::nextTrack();
      break;
    case Action::PreviousChapter:
      AudioPlayer::previousChapter();
      break;
    case Action::NextChapter:
      AudioPlayer::nextChapter();
      break;
    case Action::ShowChapters:
      chapterList = AudioPlayer::chapters();
      // Open on the page with the chapter playing.
      chaptersFirst = v.chapter >= 0 ? v.chapter / listRows() * listRows() : 0;
      showView(View::Chapters);
      return;
    case Action::OpenChapter:
      AudioPlayer::seekChapter(index);
      showView(View::NowPlaying);
      return;
    case Action::ChaptersPageUp:
    case Action::ChaptersPageDown: {
      const int rows = listRows();
      chaptersFirst += action == Action::ChaptersPageDown ? rows : -rows;
      chaptersFirst = std::max(0, std::min(chaptersFirst, std::max(0, static_cast<int>(chapterList.size()) - 1)));
      break;
    }
    case Action::VolumeDown:
      AudioPlayer::setVolume(static_cast<uint8_t>(std::max(0, v.volume - VOLUME_STEP)));
      break;
    case Action::VolumeUp:
      AudioPlayer::setVolume(static_cast<uint8_t>(std::min(100, v.volume + VOLUME_STEP)));
      break;
    case Action::ShowBooks:
      showView(View::Books);
      return;
    case Action::ShowOutput:
      showView(View::Output);
      return;
    case Action::ShowStats:
      onGoToListeningStats();
      return;
    case Action::ShowAudioModule:
      onGoToAudioModule();
      return;
    case Action::ShowAudiobookshelf:
      onGoToAudiobookshelf();
      return;
    case Action::OpenBook:
      if (index >= 0 && index < static_cast<int>(books.size())) {
        AudioPlayer::open(books[index]);
        showView(View::NowPlaying);
        return;
      }
      break;
    case Action::ShowDownloads:
      onOpenAbsDownloads();
      return;
    case Action::ResumeRecent:
      if (index >= 0 && index < static_cast<int>(recent.size())) {
        // On the card: play it here. Else fetch it from Audiobookshelf and stream it.
        if (AudioPlayer::openRecent(recent[index])) {
          showView(View::NowPlaying);
        } else if (!recent[index].itemId.empty()) {
          onResumeAudiobookshelf(recent[index].itemId);
        }
        return;
      }
      break;
    case Action::BooksPageUp:
    case Action::BooksPageDown: {
      const int rows = bookRows(contentTop);
      booksFirst += action == Action::BooksPageDown ? rows : -rows;
      booksFirst = std::max(0, std::min(booksFirst, std::max(0, static_cast<int>(books.size()) - 1)));
      break;
    }
    case Action::UseBluetooth:
      AudioPlayer::selectOutput(audiolink::Output::Bluetooth);
      break;
    case Action::UseWired:
      AudioPlayer::selectOutput(audiolink::Output::I2s);
      break;
    case Action::UseTest:
      AudioPlayer::selectOutput(audiolink::Output::Null);
      break;
    case Action::ToggleScan:
      scanning = !scanning;
      AudioPlayer::btScan(scanning);
      break;
    case Action::Forget:
      AudioPlayer::btForget();
      break;
    case Action::Connect: {
      const auto devices = AudioLink::btDevices();
      if (index >= 0 && index < static_cast<int>(devices.size())) {
        if (scanning) AudioPlayer::btScan(false);
        scanning = false;
        AudioPlayer::btConnect(devices[index].address);
      }
      break;
    }
    case Action::None:
      break;
  }
  updateRequired = true;
}

void AudioPlayerActivity::button(const int x, const int y, const int w, const int h, const char* label,
                                 const Action action, const int index, const bool filled) {
  if (filled) {
    renderer.rectangle.fill(x, y, w, h, true, true);
  } else {
    renderer.rectangle.render(x, y, w, h, true, true);
  }
  const int font = systemFontId();
  const std::string text = renderer.text.truncate(font, label, w - 16);
  const int textW = renderer.text.getWidth(font, text.c_str());
  const int textY = y + (h - renderer.text.getLineHeight(font)) / 2;
  renderer.text.render(font, x + (w - textW) / 2, textY, text.c_str(), !filled);
  targets.push_back({x, y, w, h, action, index});
}

void AudioPlayerActivity::render() {
  ScreenComponents::forgetWifiShown();
  targets.clear();
  renderer.clearScreen();
  const char* title = view == View::Books      ? "Audiobooks"
                      : view == View::Output   ? "Audio Output"
                      : view == View::Chapters ? "Chapters"
                                               : "Now Playing";
  const int top = SubPage::header(renderer, title) + 16;
  contentTop = top;
  switch (view) {
    case View::NowPlaying:
      renderNowPlaying(top);
      break;
    case View::Books:
      renderBooks(top);
      break;
    case View::Output:
      renderOutput(top);
      break;
    case View::Chapters:
      renderChapters(top);
      break;
  }
  renderer.displayBuffer();
  lastRenderMs = millis();
  renderedSecond = AudioPlayer::view().positionMs / 1000;
  renderedState = stateKey();
}

void AudioPlayerActivity::renderNowPlaying(int y) {
  const AudioPlayer::View v = AudioPlayer::view();
  const int width = renderer.getScreenWidth();
  const int inner = width - 2 * MARGIN;
  const int font = systemFontId();
  constexpr int titleFont = MONTSERRAT_18_FONT_ID;
  constexpr int smallFont = MONTSERRAT_10_FONT_ID;

  if (!v.open) {
    renderer.text.render(font, MARGIN, y, "Nothing playing.");
    y += renderer.text.getLineHeight(font) + 2 * GAP;
    button(MARGIN, y, inner, BUTTON_HEIGHT, "Choose a book", Action::ShowBooks);
    y += BUTTON_HEIGHT + GAP;
    button(MARGIN, y, inner, BUTTON_HEIGHT, "Audio output", Action::ShowOutput);
    y += BUTTON_HEIGHT + GAP;
    button(MARGIN, y, inner, BUTTON_HEIGHT, "Audio module", Action::ShowAudioModule);
    return;
  }

  // The cover at the top left, the title and chapter beside it; the rest full width under both.
  const int blockTop = y;
  const bool hasCover = !v.coverPath.empty();
  const int coverSize = renderer.getScreenHeight() >= 800 ? 200 : 120;
  int textX = MARGIN;
  int textW = inner;
  if (hasCover) {
    bool drawn = false;
    bool onCard = false;
    {
      SdIoMutex::Lock lock;
      onCard = SdMan.exists(v.coverPath.c_str());
    }
    if (onCard) {
      ImageRender::Options options;
      options.cropToFill = true;
      drawn = ImageRender::create(renderer, v.coverPath).render(MARGIN, y, coverSize, coverSize, options);
    }
    if (!drawn) renderer.rectangle.dotted(MARGIN, y, coverSize, coverSize, true);  // still being fetched
    textX = MARGIN + coverSize + GAP;
    textW = inner - coverSize - GAP;
  }
  // Up to two lines of title: whole words on the first, the rest truncated on the second.
  std::string first;
  std::string rest = v.title;
  for (size_t at = 0; at <= v.title.size();) {
    const size_t space = v.title.find(' ', at);
    const size_t end = space == std::string::npos ? v.title.size() : space;
    const std::string candidate = v.title.substr(0, end);
    if (renderer.text.getWidth(titleFont, candidate.c_str()) > textW) break;
    first = candidate;
    rest = end < v.title.size() ? v.title.substr(end + 1) : "";
    if (space == std::string::npos) break;
    at = space + 1;
  }
  if (first.empty()) {
    first = renderer.text.truncate(titleFont, v.title.c_str(), textW);
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

  // With chapters, the time and bar are the chapter's and the buttons step through chapters;
  // without, the track's (a folder's files).
  const bool chaptered = v.chapter >= 0;
  if (chaptered) {
    char line[200];
    snprintf(line, sizeof(line), "Chapter %d of %d%s%s", v.chapter + 1, v.chapterCount,
             v.chapterTitle.empty() ? "" : ": ", v.chapterTitle.c_str());
    renderer.text.render(font, textX, y, renderer.text.truncate(font, line, textW).c_str(), true, EpdFontFamily::BOLD);
    y += renderer.text.getLineHeight(font) + GAP;
  } else if (v.trackCount > 1) {
    char line[160];
    snprintf(line, sizeof(line), "Track %d of %d: %s", v.track + 1, v.trackCount, v.trackName.c_str());
    renderer.text.render(smallFont, textX, y, renderer.text.truncate(smallFont, line, textW).c_str());
    y += renderer.text.getLineHeight(smallFont) + GAP;
  }
  if (hasCover) y = std::max(y, blockTop + coverSize + GAP);

  const uint32_t shownPosition = chaptered ? v.chapterPositionMs : v.positionMs;
  const uint32_t shownDuration = chaptered ? v.chapterDurationMs : v.durationMs;
  const std::string times =
      formatTime(shownPosition) + (shownDuration ? " / " + formatTime(shownDuration) : std::string());
  renderer.text.render(font, MARGIN, y, times.c_str());
  y += renderer.text.getLineHeight(font) + GAP;
  renderer.rectangle.render(MARGIN, y, inner, 14, true);
  if (shownDuration) {
    const int filled =
        static_cast<int>(static_cast<uint64_t>(inner - 4) * std::min(shownPosition, shownDuration) / shownDuration);
    if (filled > 0) renderer.rectangle.fill(MARGIN + 2, y + 2, filled, 10, true);
  }
  y += 14 + GAP;
  if (chaptered && v.bookDurationMs) {
    const std::string book = "Book: " + formatTime(v.bookPositionMs) + " / " + formatTime(v.bookDurationMs) + "  (" +
                             std::to_string(static_cast<int>(100ull * v.bookPositionMs / v.bookDurationMs)) + "%)";
    renderer.text.render(smallFont, MARGIN, y, book.c_str());
    y += renderer.text.getLineHeight(smallFont);
  }
  y += GAP;

  // Transport
  const int buttonW = (inner - 4 * GAP) / 5;
  const char* playLabel = v.finished ? "Again" : (v.paused ? "Play" : "Pause");
  const Action actions[5] = {chaptered ? Action::PreviousChapter : Action::PreviousTrack, Action::Back30,
                             Action::PlayPause, Action::Forward30, chaptered ? Action::NextChapter : Action::NextTrack};
  const char* labels[5] = {"|<", "-30s", playLabel, "+30s", ">|"};
  for (int i = 0; i < 5; ++i) {
    button(MARGIN + i * (buttonW + GAP), y, buttonW, BUTTON_HEIGHT, labels[i], actions[i], 0, i == 2);
  }
  y += BUTTON_HEIGHT + GAP;

  // Volume
  const int small = BUTTON_HEIGHT + 20;
  button(MARGIN, y, small, BUTTON_HEIGHT, "-", Action::VolumeDown);
  button(width - MARGIN - small, y, small, BUTTON_HEIGHT, "+", Action::VolumeUp);
  char volume[24];
  snprintf(volume, sizeof(volume), "Volume %u%%", v.volume);
  const int volumeW = renderer.text.getWidth(font, volume);
  renderer.text.render(font, (width - volumeW) / 2, y + (BUTTON_HEIGHT - renderer.text.getLineHeight(font)) / 2,
                       volume);
  y += BUTTON_HEIGHT + GAP;

  char output[96];
  if (v.output == audiolink::Output::Bluetooth) {
    snprintf(output, sizeof(output), "Output: Bluetooth, %s", btStateName(v.btState));
  } else {
    snprintf(output, sizeof(output), "Output: %s", outputName(v.output));
  }
  button(MARGIN, y, inner, BUTTON_HEIGHT, output, Action::ShowOutput);
  y += BUTTON_HEIGHT + GAP;
  // Chapters (when the book has them), Books, and the audio module's page (Device Management).
  {
    const int columns = chaptered ? 3 : 2;
    const int w = (inner - (columns - 1) * GAP) / columns;
    int x = MARGIN;
    if (chaptered) {
      button(x, y, w, BUTTON_HEIGHT, "Chapters", Action::ShowChapters);
      x += w + GAP;
    }
    button(x, y, w, BUTTON_HEIGHT, "Books", Action::ShowBooks);
    x += w + GAP;
    button(x, y, MARGIN + inner - x, BUTTON_HEIGHT, "Module", Action::ShowAudioModule);
  }
  y += BUTTON_HEIGHT + 2 * GAP;

  renderer.text.render(font, MARGIN, y, moduleStateName(v));
  y += renderer.text.getLineHeight(font) + GAP;
  if (!v.message.empty()) {
    // Wrapped at spaces, up to three lines: a network error says which networks failed and why.
    constexpr int MAX_MESSAGE_LINES = 3;
    const int lineHeight = renderer.text.getLineHeight(smallFont);
    std::string rest = v.message;
    for (int line = 0; line < MAX_MESSAGE_LINES && !rest.empty(); ++line) {
      size_t take = rest.size();
      if (line + 1 < MAX_MESSAGE_LINES) {
        while (take > 0 && renderer.text.getWidth(smallFont, rest.substr(0, take).c_str()) > inner) {
          const size_t space = rest.rfind(' ', take - 1);
          take = space == std::string::npos || space == 0 ? 0 : space;
        }
        if (take == 0) take = rest.size();  // one word wider than the line: truncated below
      }
      renderer.text.render(smallFont, MARGIN, y + line * lineHeight,
                           renderer.text.truncate(smallFont, rest.substr(0, take).c_str(), inner).c_str());
      rest = take < rest.size() ? rest.substr(take + 1) : "";
    }
  }
}

/** Rows of a list view (Chapters): the same count paging and rendering use. */
int AudioPlayerActivity::listRows() const {
  const int pagerTop = renderer.getScreenHeight() - BUTTON_HEIGHT - 2 * GAP;
  return std::max(1, (pagerTop - contentTop) / ROW_HEIGHT);
}

void AudioPlayerActivity::renderChapters(int y) {
  const int width = renderer.getScreenWidth();
  const int inner = width - 2 * MARGIN;
  const int font = systemFontId();
  constexpr int smallFont = MONTSERRAT_10_FONT_ID;
  const AudioPlayer::View v = AudioPlayer::view();
  const int pagerTop = renderer.getScreenHeight() - BUTTON_HEIGHT - 2 * GAP;
  const int rows = listRows();
  const int count = static_cast<int>(chapterList.size());
  for (int i = 0; i < rows && chaptersFirst + i < count; ++i) {
    const int c = chaptersFirst + i;
    const AudioChapter& chapter = chapterList[c];
    const int rowY = y + i * ROW_HEIGHT;
    const bool current = c == v.chapter;
    // Start time on the right; the title gets the rest of the row.
    const std::string start = formatTime(chapter.startMs);
    const int startW = renderer.text.getWidth(smallFont, start.c_str());
    std::string label =
        (current ? "> " : "") + (chapter.title.empty() ? "Chapter " + std::to_string(c + 1) : chapter.title);
    label = renderer.text.truncate(font, label.c_str(), inner - startW - GAP);
    renderer.text.render(font, MARGIN, rowY + (ROW_HEIGHT - renderer.text.getLineHeight(font)) / 2, label.c_str(), true,
                         current ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
    renderer.text.render(smallFont, MARGIN + inner - startW,
                         rowY + (ROW_HEIGHT - renderer.text.getLineHeight(smallFont)) / 2, start.c_str());
    renderer.line.render(0, rowY + ROW_HEIGHT - 1, width, rowY + ROW_HEIGHT - 1, true, LineRender::Style::Dotted);
    targets.push_back({0, rowY, width, ROW_HEIGHT, Action::OpenChapter, c});
  }
  const int pagerW = (inner - GAP) / 2;
  if (chaptersFirst > 0) button(MARGIN, pagerTop, pagerW, BUTTON_HEIGHT, "Previous", Action::ChaptersPageUp);
  if (chaptersFirst + rows < count) {
    button(MARGIN + pagerW + GAP, pagerTop, pagerW, BUTTON_HEIGHT, "Next", Action::ChaptersPageDown);
  }
}

void AudioPlayerActivity::drawCover(const std::string& path, const int x, const int y, const int size) {
  bool onCard = false;
  if (!path.empty()) {
    SdIoMutex::Lock lock;
    onCard = SdMan.exists(path.c_str());
  }
  if (onCard) {
    ImageRender::Options options;
    options.cropToFill = true;
    if (ImageRender::create(renderer, path).render(x, y, size, size, options)) return;
  }
  renderer.rectangle.dotted(x, y, size, size, true);
}

/** Device books shown per page: what fits under the Audiobookshelf button and recent streams. */
int AudioPlayerActivity::bookRows(const int top) const {
  const int labelH = 40;
  int used = BUTTON_HEIGHT + 2 * GAP;
  if (!recent.empty()) used += labelH + static_cast<int>(recent.size()) * COVER_ROW_HEIGHT + GAP;
  used += labelH;
  const int pagerTop = renderer.getScreenHeight() - BUTTON_HEIGHT - 2 * GAP;
  return std::max(1, (pagerTop - top - used) / COVER_ROW_HEIGHT);
}

void AudioPlayerActivity::renderBooks(int y) {
  const int width = renderer.getScreenWidth();
  const int inner = width - 2 * MARGIN;
  const int font = systemFontId();
  constexpr int smallFont = MONTSERRAT_10_FONT_ID;
  const int top = y;

  // Audiobookshelf, its download queue (with how many books are in it), and listening stats.
  const int third = (inner - 2 * GAP) / 3;
  button(MARGIN, y, third, BUTTON_HEIGHT, "Audiobookshelf", Action::ShowAudiobookshelf);
  const size_t queued = AbsQueue::entries().size();
  const std::string downloads = queued ? "Downloads (" + std::to_string(queued) + ")" : "Downloads";
  button(MARGIN + third + GAP, y, third, BUTTON_HEIGHT, downloads.c_str(), Action::ShowDownloads);
  button(MARGIN + 2 * (third + GAP), y, inner - 2 * (third + GAP), BUTTON_HEIGHT, "Stats", Action::ShowStats);
  y += BUTTON_HEIGHT + 2 * GAP;

  const auto label = [&](const char* text) {
    renderer.text.render(smallFont, MARGIN, y + 8, text, true, EpdFontFamily::BOLD);
    y += 40;
  };
  const int cover = COVER_ROW_HEIGHT - 2 * COVER_INSET;
  const int textX = MARGIN + cover + GAP;
  const int textW = inner - cover - GAP;
  const auto bookRow = [&](const std::string& coverPath, const std::string& title, const std::string& info,
                           const bool current, const Action action, const int index) {
    drawCover(coverPath, MARGIN, y + COVER_INSET, cover);
    renderer.text.render(font, textX, y + 10,
                         renderer.text.truncate(font, ((current ? "> " : "") + title).c_str(), textW).c_str(), true,
                         EpdFontFamily::BOLD);
    if (!info.empty()) {
      renderer.text.render(smallFont, textX, y + 16 + renderer.text.getLineHeight(font),
                           renderer.text.truncate(smallFont, info.c_str(), textW).c_str());
    }
    renderer.line.render(0, y + COVER_ROW_HEIGHT - 1, width, y + COVER_ROW_HEIGHT - 1, true, LineRender::Style::Dotted);
    targets.push_back({0, y, width, COVER_ROW_HEIGHT, action, index});
    y += COVER_ROW_HEIGHT;
  };
  const AudioPlayer::View v = AudioPlayer::view();

  if (!recent.empty()) {
    label("RECENT");
    for (size_t i = 0; i < recent.size(); ++i) {
      const AudioPlayer::RecentAudiobook& r = recent[i];
      std::string info;
      if (r.durationMs) info = std::to_string(static_cast<int>(100ull * r.positionMs / r.durationMs)) + "%";
      if (!r.author.empty()) info += (info.empty() ? "" : "  -  ") + r.author;
      if (r.path.empty()) info += info.empty() ? "Streams" : "  -  streams";
      bookRow(r.coverPath, r.title, info, v.open && v.title == r.title, Action::ResumeRecent, static_cast<int>(i));
    }
    y += GAP;
  }

  label("ON THIS DEVICE");
  if (books.empty()) {
    renderer.text.render(font, MARGIN, y, "No audiobooks on the device.");
    y += renderer.text.getLineHeight(font) + GAP;
    renderer.text.render(smallFont, MARGIN, y, "Download them from Audiobookshelf, or copy them to");
    y += renderer.text.getLineHeight(smallFont) + 4;
    renderer.text.render(smallFont, MARGIN, y, "/Audiobooks: one .m4b/.m4a/.mp3 file or folder per book.");
    return;
  }
  const int rows = bookRows(top);
  for (int i = 0; i < rows && booksFirst + i < static_cast<int>(books.size()); ++i) {
    const int n = booksFirst + i;
    const AudioPlayer::Book& book = books[n];
    const bool current = v.open && !v.remote && book.title == v.title;
    bookRow(n < static_cast<int>(bookCovers.size()) ? bookCovers[n] : "", book.title,
            book.absItemId.empty() ? "" : "From Audiobookshelf", current, Action::OpenBook, n);
  }

  const int pagerTop = renderer.getScreenHeight() - BUTTON_HEIGHT - 2 * GAP;
  const int pagerW = (inner - GAP) / 2;
  if (booksFirst > 0) button(MARGIN, pagerTop, pagerW, BUTTON_HEIGHT, "Previous", Action::BooksPageUp);
  if (booksFirst + rows < static_cast<int>(books.size())) {
    button(MARGIN + pagerW + GAP, pagerTop, pagerW, BUTTON_HEIGHT, "Next", Action::BooksPageDown);
  }
}

int AudioPlayerActivity::paragraph(const int fontId, const int x, int y, const int maxWidth, const std::string& text) {
  const int lineHeight = renderer.text.getLineHeight(fontId);
  std::string line;
  const auto flush = [&] {
    renderer.text.render(fontId, x, y, renderer.text.truncate(fontId, line.c_str(), maxWidth).c_str());
    y += lineHeight;
    line.clear();
  };
  for (size_t at = 0; at < text.size();) {
    const size_t space = text.find(' ', at);
    const std::string word = text.substr(at, space == std::string::npos ? std::string::npos : space - at);
    at = space == std::string::npos ? text.size() : space + 1;
    const std::string candidate = line.empty() ? word : line + " " + word;
    if (!line.empty() && renderer.text.getWidth(fontId, candidate.c_str()) > maxWidth) {
      flush();
      line = word;
    } else {
      line = candidate;
    }
  }
  if (!line.empty()) flush();
  return y;
}

void AudioPlayerActivity::renderOutput(int y) {
  const AudioPlayer::View v = AudioPlayer::view();
  const AudioLink::ModuleStatus module = AudioLink::status();
  const int width = renderer.getScreenWidth();
  const int inner = width - 2 * MARGIN;
  const int font = systemFontId();
  constexpr int smallFont = MONTSERRAT_10_FONT_ID;

  if (!module.valid) {
    y = paragraph(font, MARGIN, y, inner, "Waiting for the audio module...") + GAP;
    paragraph(smallFont, MARGIN, y, inner, "Plug it into the USB-C port.");
    return;
  }

  const Action actions[3] = {Action::UseBluetooth, Action::UseWired, Action::UseTest};
  const audiolink::Output outputs[3] = {audiolink::Output::Bluetooth, audiolink::Output::I2s, audiolink::Output::Null};
  const char* labels[3] = {"Bluetooth headphones", "Wired (DAC on the module)", "Test (no sound)"};
  for (int i = 0; i < 3; ++i) {
    button(MARGIN, y, inner, BUTTON_HEIGHT, labels[i], actions[i], 0, v.output == outputs[i]);
    y += BUTTON_HEIGHT + GAP;
  }
  if (v.output != audiolink::Output::Bluetooth) return;

  y += GAP;
  const bool remembered = module.status.btAddress[0] | module.status.btAddress[1] | module.status.btAddress[2] |
                          module.status.btAddress[3] | module.status.btAddress[4] | module.status.btAddress[5];
  std::string state = std::string("Headphones: ") + btStateName(v.btState);
  if (remembered) state += " (" + formatAddress(module.status.btAddress) + ")";
  y = paragraph(smallFont, MARGIN, y, inner, state) + GAP;

  const int halfW = (inner - GAP) / 2;
  button(MARGIN, y, halfW, BUTTON_HEIGHT, scanning ? "Stop scan" : "Scan", Action::ToggleScan, 0, scanning);
  if (remembered) button(MARGIN + halfW + GAP, y, halfW, BUTTON_HEIGHT, "Forget", Action::Forget);
  y += BUTTON_HEIGHT + GAP;

  if (scanning) {
    y = paragraph(smallFont, MARGIN, y, inner, "Put the headphones in pairing mode, then tap them below.") + GAP;
  }
  const auto devices = AudioLink::btDevices();
  const int bottom = renderer.getScreenHeight() - GAP;
  for (size_t i = 0; i < devices.size() && y + ROW_HEIGHT <= bottom; ++i) {
    char line[80];
    snprintf(line, sizeof(line), "%s  (%d dBm)", devices[i].name.empty() ? "(no name)" : devices[i].name.c_str(),
             devices[i].rssi);
    renderer.text.render(font, MARGIN, y + (ROW_HEIGHT - renderer.text.getLineHeight(font)) / 2,
                         renderer.text.truncate(font, line, inner).c_str());
    renderer.line.render(0, y + ROW_HEIGHT - 1, width, y + ROW_HEIGHT - 1, true, LineRender::Style::Dotted);
    targets.push_back({0, y, width, ROW_HEIGHT, Action::Connect, static_cast<int>(i)});
    y += ROW_HEIGHT;
  }
}

#endif  // INX_HAS_AUDIO_MODULE
