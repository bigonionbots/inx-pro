#pragma once

/**
 * @file UsbAudioOut.h
 * @brief USB headphone adapters (USB Audio Class 1.0) on the T5's USB-C port, in place of the
 *        audio module (LilyGo T5 S3, t5s3_audio only).
 *
 * When the USB link is on (AudioModule::setLinkEnabled), the UAC driver
 * (third_party/usb_host/usb_host_uac) shares the USB host stack with the serial bridge drivers. An
 * adapter that enumerates is opened and its output formats go to the module log. Books play
 * through it via UsbAudioSink, which decodes on the T5 and writes 16-bit stereo PCM with the
 * stream calls below; the Audio Module screen can also play a test tone (left, right, then both
 * channels, quietly).
 *
 * Threading: the stream calls may come from any task; one lock keeps them and the adapter's
 * open/close (on this module's task) apart. streamWrite() blocks until the driver's buffer
 * (about 45 ms) has room, which is what paces playback to the adapter's clock.
 *
 * Tested with a Sabrent AU-UCMA (C-Media 0D8C:0147: full speed, synchronous OUT endpoint,
 * 16/24-bit stereo at 44.1/48/96 kHz). UAC 2.0-only adapters are not supported by the driver.
 */

#if INX_HAS_AUDIO_MODULE

#include <cstdint>

namespace UsbAudioOut {

enum class State : uint8_t {
  Off,      ///< Driver not installed (USB link off)
  Waiting,  ///< Driver installed, no adapter
  Ready,    ///< An adapter is open
  Playing,  ///< The test tone is playing
  Failed,   ///< Driver install failed; see the module log
};

struct DeviceInfo {
  uint16_t vid = 0;
  uint16_t pid = 0;
  char product[40] = {};
  /** Output formats, e.g. "16/24-bit 2ch 44100 48000 96000". */
  char formats[64] = {};
  /** Rates the adapter takes for 16-bit stereo, ascending. */
  uint32_t rates[8] = {};
  uint8_t rateCount = 0;
};

struct Counters {
  uint32_t tones = 0;           ///< Test tones finished
  uint32_t lastToneBytes = 0;   ///< Bytes handed to the driver by the last tone
  uint32_t lastToneMs = 0;      ///< How long the adapter took to take them
  uint32_t writeTimeouts = 0;   ///< Writes the driver did not accept in time (adapter not draining)
  uint32_t transferErrors = 0;  ///< Isochronous transfer errors reported by the driver
};

/**
 * Registers the UAC driver on the running USB host stack. UsbSerialHost::start() calls it after
 * usb_host_install() and before powering the root port, so an adapter that is already plugged in
 * is seen. False if the driver could not be installed (the serial link still works).
 */
bool install();

/** Closes the adapter and deregisters the driver. UsbSerialHost::stop() calls it first. */
void uninstall();

State state();
bool isConnected();
DeviceInfo device();
Counters counters();

/** Starts the test tone (about 3 s) on the adapter's own task. False when no adapter is ready or a stream plays. */
bool playTestTone();

/** Increments every time an adapter is opened, so a player knows to start its stream again. */
uint32_t connectCounter();

/** The adapter rate closest to `sourceRate` that keeps resampling simple; 0 without an adapter. */
uint32_t bestRate(uint32_t sourceRate);

/** Starts 16-bit stereo output at `rate` (one of DeviceInfo::rates). True if already running at it. */
bool streamStart(uint32_t rate);
/** Writes stereo frames, waiting up to timeoutMs for room. Returns the frames the driver took. */
size_t streamWrite(const int16_t* frames, size_t count, uint32_t timeoutMs);
/** Stops output (what is buffered is dropped). */
void streamStop();
/** The running stream's rate, 0 if none. */
uint32_t streamRate();
/** PCM waiting in the driver, in ms (what streamWrite() is ahead of the speaker). */
uint32_t streamBufferedMs();
/** The adapter's volume, 0-100 across its own range (0 mutes). Needs a running stream. */
bool setVolume(uint8_t percent);

}  // namespace UsbAudioOut

#endif  // INX_HAS_AUDIO_MODULE
