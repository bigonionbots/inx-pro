#pragma once

/**
 * @file AudioPlayer.h
 * @brief Audiobook playback from the SD card through the external audio module.
 *
 * A book is an audio file (.m4b / .m4a / .mp4 / .mp3) or a folder of them (played in natural name
 * order) under /Audiobooks. Progress (track and position) is kept per book in
 * /.system/audio_progress.txt.
 *
 * Threading: everything here runs on the main loop task. service() reads the SD card (holding
 * SdIoMutex) in short slices into a large PSRAM queue, so file access never races the reader's SD
 * use. A sender task drains that queue into AudioLink, which blocks on the module's buffer credit,
 * and touches neither the card nor this module's state. The queue holds about a minute of audio,
 * which rides out long main-loop stalls such as EPUB page layout.
 */

#if INX_HAS_AUDIO_MODULE

#include <AudioLinkProtocol.h>

#include <cstdint>
#include <string>
#include <vector>

namespace AudioPlayer {

constexpr const char* BOOKS_DIRECTORY = "/Audiobooks";

struct Book {
  std::string path;
  std::string title;
  bool folder = false;
};

struct View {
  bool open = false;
  bool paused = false;
  bool finished = false;
  /** Module not connected or not answering yet; playback resumes by itself when it is. */
  bool waitingForModule = false;
  std::string title;
  std::string trackName;
  int track = 0;  ///< 0-based
  int trackCount = 0;
  uint32_t positionMs = 0;  ///< within the track
  uint32_t durationMs = 0;  ///< of the track, 0 if unknown
  audiolink::PlayState moduleState = audiolink::PlayState::Idle;
  audiolink::BtState btState = audiolink::BtState::Off;
  audiolink::Output output = audiolink::Output::Bluetooth;
  uint8_t volume = 0;
  uint32_t queuedBytes = 0;  ///< audio read from the card, not yet sent to the module
  std::string message;    ///< last error or notice
};

/** Starts the sender task. Call once at boot. */
void begin();

std::vector<Book> listBooks();
/** Opens a book at its saved position and starts playing. Turns the module link and power on. */
bool open(const Book& book);
void stop();

void togglePause();
void skip(int32_t deltaMs);
void nextTrack();
void previousTrack();
void setVolume(uint8_t percent);
void selectOutput(audiolink::Output output);

/** Turns the module link and power on (retried at most every few seconds). */
void wakeModule();
void btScan(bool start);
void btConnect(const uint8_t address[6]);
void btForget();

View view();
/** True while a book is playing, so the device should not go to sleep. */
bool keepAwake();

/** Main-loop housekeeping: SD reads into the queue, track changes, progress saving. */
void service();
/** Before deep sleep: saves progress and stops the stream. */
void shutdown();

}  // namespace AudioPlayer

#endif  // INX_HAS_AUDIO_MODULE
