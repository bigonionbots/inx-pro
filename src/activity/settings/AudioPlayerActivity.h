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
 * Audiobook player screen (Home -> Shortcuts -> Audiobooks). Three views:
 * - Now playing: transport, volume and output; playback keeps going after the screen closes.
 * - Books (the Audiobooks page): recently streamed Audiobookshelf books (tap to carry on), the
 *   books on the device under /Audiobooks, and the way into Audiobookshelf (AbsActivity).
 * - Chapters: the open book's chapters, to jump to one (books with chapters only).
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
  enum class View : uint8_t { NowPlaying, Books, Output, Chapters };

  enum class Action : uint8_t {
    None,
    PlayPause,
    Back30,
    Forward30,
    PreviousTrack,
    NextTrack,
    PreviousChapter,
    NextChapter,
    ShowChapters,
    OpenChapter,
    ChaptersPageUp,
    ChaptersPageDown,
    VolumeDown,
    VolumeUp,
    ShowBooks,
    ShowOutput,
    ShowAudiobookshelf,
    ShowAudioModule,  ///< Device Management's Audio module page, back to here
    OpenBook,
    ResumeStreamed,  ///< index = position in `recent`
    ShowDownloads,
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
  std::vector<AudioPlayer::RecentStream> recent;  ///< while the Books view is open
  std::vector<std::string> bookCovers;            ///< per book, "" when none (found once per visit)
  int booksFirst = 0;
  std::vector<AudioChapter> chapterList;  ///< while the Chapters view is open
  int chaptersFirst = 0;
  int contentTop = 120;  ///< where views start under the header, from the last render
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
  void renderChapters(int top);
  void drawCover(const std::string& path, int x, int y, int size);
  int bookRows(int top) const;
  /** Word-wrapped text from (x, y) within maxWidth; returns the y under its last line. */
  int paragraph(int fontId, int x, int y, int maxWidth, const std::string& text);
  int listRows() const;
  void button(int x, int y, int w, int h, const char* label, Action action, int index = 0, bool filled = false);
  std::string stateKey() const;
};

#endif  // INX_HAS_AUDIO_MODULE
