#pragma once

/**
 * @file AudioPlayerActivity.h
 * @brief Public interface and types for AudioPlayerActivity.
 */

#if INX_HAS_AUDIO_MODULE

#include <functional>
#include <string>
#include <vector>

#include "activity/Activity.h"
#include "system/AudioPlayer.h"

/**
 * Audiobook player screen (Device Management -> Audiobooks). Three views:
 * - Now playing: transport, volume and output; playback keeps going after the screen closes.
 * - Books: the books under /Audiobooks.
 * - Output: Bluetooth headphones (scan, connect, forget), the wired DAC, or the silent test sink.
 *
 * Confirm toggles play/pause and Up/Down change the volume; everything else is touch. While a
 * book plays the screen redraws its position every few seconds with a fast refresh.
 */
class AudioPlayerActivity final : public Activity {
 public:
  explicit AudioPlayerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                               const std::function<void()>& onClose)
      : Activity("AudioPlayer", renderer, mappedInput), onClose(onClose) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  bool preventAutoSleep() override { return AudioPlayer::keepAwake(); }

 private:
  enum class View : uint8_t { NowPlaying, Books, Output };

  enum class Action : uint8_t {
    None,
    PlayPause,
    Back30,
    Forward30,
    PreviousTrack,
    NextTrack,
    VolumeDown,
    VolumeUp,
    ShowBooks,
    ShowOutput,
    OpenBook,
    BooksPageUp,
    BooksPageDown,
    UseBluetooth,
    UseWired,
    UseTest,
    ToggleScan,
    Forget,
    Connect,
  };

  struct Target {
    int x, y, w, h;
    Action action;
    int index;
  };

  const std::function<void()> onClose;
  View view = View::NowPlaying;
  std::vector<AudioPlayer::Book> books;
  int booksFirst = 0;
  bool scanning = false;
  std::vector<Target> targets;
  bool updateRequired = false;
  unsigned long lastRenderMs = 0;
  uint32_t renderedSecond = UINT32_MAX;
  std::string renderedState;

  void run(Action action, int index);
  void showView(View next);
  void render();
  void renderNowPlaying(int top);
  void renderBooks(int top);
  void renderOutput(int top);
  void button(int x, int y, int w, int h, const char* label, Action action, int index = 0, bool filled = false);
  std::string stateKey() const;
};

#endif  // INX_HAS_AUDIO_MODULE
