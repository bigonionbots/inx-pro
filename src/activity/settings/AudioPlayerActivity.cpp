/**
 * @file AudioPlayerActivity.cpp
 * @brief Definitions for AudioPlayerActivity.
 */

#include "AudioPlayerActivity.h"

#if INX_HAS_AUDIO_MODULE

#include <GfxRenderer.h>

#include <algorithm>
#include <cstdio>

#include "activity/page/SubPage.h"
#include "system/AudioLink.h"
#include "system/Fonts.h"
#include "system/MappedInputManager.h"
#include "system/UiLayout.h"

namespace {

constexpr int MARGIN = 20;
constexpr int ROW_HEIGHT = UiLayout::LIST_ITEM_HEIGHT;
constexpr int BUTTON_HEIGHT = 76;
constexpr int GAP = 12;
constexpr int VOLUME_STEP = 10;
constexpr int32_t SKIP_MS = 30000;
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
  showView(AudioPlayer::view().open ? View::NowPlaying : View::Books);
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
    run(view == View::Books ? Action::BooksPageUp : Action::VolumeUp, 0);
    return;
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Down)) {
    run(view == View::Books ? Action::BooksPageDown : Action::VolumeDown, 0);
    return;
  }

  const unsigned long now = millis();
  const AudioPlayer::View v = AudioPlayer::view();
  const std::string key = stateKey();
  const bool stateChanged = key != renderedState && now - lastRenderMs >= MIN_REDRAW_MS;
  const bool positionStale = view == View::NowPlaying && v.positionMs / 1000 != renderedSecond &&
                             now - lastRenderMs >= POSITION_REDRAW_MS;
  if (updateRequired || stateChanged || positionStale) {
    updateRequired = false;
    render();
  }
}

std::string AudioPlayerActivity::stateKey() const {
  const AudioPlayer::View v = AudioPlayer::view();
  char key[160];
  snprintf(key, sizeof(key), "%d%d%d%d|%u|%u|%u|%u|%d|%zu|%s", v.open, v.paused, v.finished, v.waitingForModule,
           static_cast<unsigned>(v.moduleState), static_cast<unsigned>(v.btState), static_cast<unsigned>(v.output),
           v.volume, v.track, view == View::Output ? AudioLink::btDevices().size() : 0, v.message.c_str());
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
    case Action::OpenBook:
      if (index >= 0 && index < static_cast<int>(books.size())) {
        AudioPlayer::open(books[index]);
        showView(View::NowPlaying);
        return;
      }
      break;
    case Action::BooksPageUp:
    case Action::BooksPageDown: {
      const int rows = std::max(1, (renderer.getScreenHeight() - 260) / ROW_HEIGHT);
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
  targets.clear();
  renderer.clearScreen();
  const char* title = view == View::Books ? "Audiobooks" : (view == View::Output ? "Audio Output" : "Now Playing");
  const int top = SubPage::header(renderer, title) + 16;
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
    return;
  }

  const std::string title = renderer.text.truncate(titleFont, v.title.c_str(), inner);
  renderer.text.render(titleFont, MARGIN, y, title.c_str(), true, EpdFontFamily::BOLD);
  y += renderer.text.getLineHeight(titleFont) + GAP;

  if (v.trackCount > 1) {
    char line[160];
    snprintf(line, sizeof(line), "Track %d of %d: %s", v.track + 1, v.trackCount, v.trackName.c_str());
    renderer.text.render(smallFont, MARGIN, y, renderer.text.truncate(smallFont, line, inner).c_str());
    y += renderer.text.getLineHeight(smallFont) + GAP;
  }

  const std::string times =
      formatTime(v.positionMs) + (v.durationMs ? " / " + formatTime(v.durationMs) : std::string());
  renderer.text.render(font, MARGIN, y, times.c_str());
  y += renderer.text.getLineHeight(font) + GAP;
  renderer.rectangle.render(MARGIN, y, inner, 14, true);
  if (v.durationMs) {
    const int filled = static_cast<int>(static_cast<uint64_t>(inner - 4) * std::min(v.positionMs, v.durationMs) /
                                        v.durationMs);
    if (filled > 0) renderer.rectangle.fill(MARGIN + 2, y + 2, filled, 10, true);
  }
  y += 14 + 2 * GAP;

  // Transport
  const int buttonW = (inner - 4 * GAP) / 5;
  const char* playLabel = v.finished ? "Again" : (v.paused ? "Play" : "Pause");
  const Action actions[5] = {Action::PreviousTrack, Action::Back30, Action::PlayPause, Action::Forward30,
                             Action::NextTrack};
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
  button(MARGIN, y, inner, BUTTON_HEIGHT, "Books", Action::ShowBooks);
  y += BUTTON_HEIGHT + 2 * GAP;

  renderer.text.render(font, MARGIN, y, moduleStateName(v));
  y += renderer.text.getLineHeight(font) + GAP;
  if (!v.message.empty()) {
    renderer.text.render(smallFont, MARGIN, y, renderer.text.truncate(smallFont, v.message.c_str(), inner).c_str());
  }
}

void AudioPlayerActivity::renderBooks(int y) {
  const int width = renderer.getScreenWidth();
  const int inner = width - 2 * MARGIN;
  const int font = systemFontId();
  constexpr int smallFont = MONTSERRAT_10_FONT_ID;

  if (books.empty()) {
    renderer.text.render(font, MARGIN, y, "No audiobooks found.");
    y += renderer.text.getLineHeight(font) + GAP;
    renderer.text.render(smallFont, MARGIN, y, "Copy them to /Audiobooks on the SD card:");
    y += renderer.text.getLineHeight(smallFont) + 4;
    renderer.text.render(smallFont, MARGIN, y, "one .m4b/.m4a/.mp3 file, or one folder per book.");
    return;
  }

  const int pagerTop = renderer.getScreenHeight() - BUTTON_HEIGHT - 2 * GAP;
  const int rows = std::max(1, (pagerTop - y) / ROW_HEIGHT);
  const AudioPlayer::View v = AudioPlayer::view();
  for (int i = 0; i < rows && booksFirst + i < static_cast<int>(books.size()); ++i) {
    const AudioPlayer::Book& book = books[booksFirst + i];
    const int rowY = y + i * ROW_HEIGHT;
    const bool current = v.open && book.title == v.title;
    std::string label = (current ? "> " : "") + book.title;
    label = renderer.text.truncate(font, label.c_str(), inner);
    renderer.text.render(font, MARGIN, rowY + (ROW_HEIGHT - renderer.text.getLineHeight(font)) / 2, label.c_str(),
                         true, current ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
    renderer.line.render(0, rowY + ROW_HEIGHT - 1, width, rowY + ROW_HEIGHT - 1, true, LineRender::Style::Dotted);
    targets.push_back({0, rowY, width, ROW_HEIGHT, Action::OpenBook, booksFirst + i});
  }

  const int pagerW = (inner - GAP) / 2;
  if (booksFirst > 0) button(MARGIN, pagerTop, pagerW, BUTTON_HEIGHT, "Previous", Action::BooksPageUp);
  if (booksFirst + rows < static_cast<int>(books.size())) {
    button(MARGIN + pagerW + GAP, pagerTop, pagerW, BUTTON_HEIGHT, "Next", Action::BooksPageDown);
  }
}

void AudioPlayerActivity::renderOutput(int y) {
  const AudioPlayer::View v = AudioPlayer::view();
  const AudioLink::ModuleStatus module = AudioLink::status();
  const int width = renderer.getScreenWidth();
  const int inner = width - 2 * MARGIN;
  const int font = systemFontId();
  constexpr int smallFont = MONTSERRAT_10_FONT_ID;

  if (!module.valid) {
    renderer.text.render(font, MARGIN, y, "Waiting for the audio module...");
    y += renderer.text.getLineHeight(font) + GAP;
    renderer.text.render(smallFont, MARGIN, y, "Plug it into the USB-C port.");
    return;
  }

  const Action actions[3] = {Action::UseBluetooth, Action::UseWired, Action::UseTest};
  const audiolink::Output outputs[3] = {audiolink::Output::Bluetooth, audiolink::Output::I2s,
                                        audiolink::Output::Null};
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
  renderer.text.render(smallFont, MARGIN, y, renderer.text.truncate(smallFont, state.c_str(), inner).c_str());
  y += renderer.text.getLineHeight(smallFont) + GAP;

  const int halfW = (inner - GAP) / 2;
  button(MARGIN, y, halfW, BUTTON_HEIGHT, scanning ? "Stop scan" : "Scan", Action::ToggleScan, 0, scanning);
  if (remembered) button(MARGIN + halfW + GAP, y, halfW, BUTTON_HEIGHT, "Forget", Action::Forget);
  y += BUTTON_HEIGHT + GAP;

  if (scanning) {
    renderer.text.render(smallFont, MARGIN, y, "Put the headphones in pairing mode, then tap them below.");
    y += renderer.text.getLineHeight(smallFont) + GAP;
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
