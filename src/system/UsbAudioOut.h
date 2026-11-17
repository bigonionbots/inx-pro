#pragma once

/**
 * @file UsbAudioOut.h
 * @brief USB headphone adapters (USB Audio Class 1.0) on the T5's USB-C port, in place of the
 *        audio module (LilyGo T5 S3, t5s3_audio only).
 *
 * The probe stage: when the USB link is on (AudioModule::setLinkEnabled), the UAC driver
 * (third_party/usb_host/usb_host_uac) shares the USB host stack with the serial bridge drivers. An
 * adapter that enumerates is opened, its output formats go to the module log, and the Audio Module
 * screen can play a test tone through it: left, right, then both channels, quietly. Playback of
 * books and game sound through it comes later.
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

/** Starts the test tone (about 3 s) on the adapter's own task. False when no adapter is ready. */
bool playTestTone();

}  // namespace UsbAudioOut

#endif  // INX_HAS_AUDIO_MODULE
