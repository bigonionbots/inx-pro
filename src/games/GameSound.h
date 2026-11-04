#pragma once

/**
 * @file GameSound.h
 * @brief Game sound through the external audio module (docs/audio-module.md), in game mode.
 *
 * With sound on (the Games screen's Sound button, stored in /Games/.sound), begin() powers the
 * module and starts the USB link as the Audio Module screen would, and a sender task streams
 * the game's sound to it as live µ-law (shared/audio_link/MuLaw.h), 22050 Hz mono. The module
 * plays it on its current output (Bluetooth or the wired DAC, as chosen for audiobooks).
 *
 * Live means late audio is dropped rather than queued, at both ends: this side keeps about
 * 90 ms, the module about LIVE_MAX_BUFFERED_MS. A stall therefore costs a gap, not a delay.
 *
 * All calls are from the game loop.
 */

#if INX_HAS_GAMES

#include <cstddef>
#include <cstdint>

namespace GameSound {

/** Output rate of the stream; push() takes twice this. */
constexpr uint32_t STREAM_RATE = 22050;

/** The Sound setting (/Games/.sound). Off unless switched on. */
bool enabled();
void setEnabled(bool on);

/** Game mode: starts the module and the link if sound is on. Returns whether it is. */
bool begin();

/** Sound is on and the module was started (begin() returned true). */
bool active();

/**
 * The module's volume, 0..100: as last set here, else as the module reports it. -1 while not
 * known (sound off, or no STATUS from the module yet).
 */
int volume();

/** Volume steps of VOLUME_STEP (0..100). Sent by the sender task, so they never block the game. */
constexpr int VOLUME_STEP = 10;
void stepVolume(bool up);

/** Mute, or back to the volume before muting. */
void toggleMute();

/** Housekeeping (the module's power checks); call every frame. */
void service();

/** Mono samples at 2 x STREAM_RATE (44.1 kHz). Never blocks: drops them if the link is behind. */
void push(const int16_t* samples, size_t count);

/**
 * For the status line, once per update: "" with sound off, "snd -" while waiting for the module,
 * else "u<underruns> d<drops>" since the last call (both 0 when the sound is clean).
 */
const char* statusText();

/** Before leaving game mode: ends the stream. */
void end();

}  // namespace GameSound

#endif  // INX_HAS_GAMES
