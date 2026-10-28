#pragma once

/**
 * @file AudioLink.h
 * @brief T5 end of the audio module link protocol (shared/audio_link/AudioLinkProtocol.h).
 *
 * A background task reads UsbSerialHost, parses module frames (ACK, STATUS, HELLO, BT_DEVICE,
 * LOG) and resends unacknowledged host frames (go-back-N). Any task may send: send() blocks until
 * the window has room, sendAudio() additionally until the module has granted enough buffer
 * credit. A session starts with PING when the bridge connects; a HELLO the T5 did not ask for
 * means the module rebooted, which bumps sessionCounter() so the player restarts its stream.
 */

#if INX_HAS_AUDIO_MODULE

#include <AudioLinkProtocol.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace AudioLink {

struct BtDevice {
  uint8_t address[6];
  int8_t rssi;
  std::string name;
};

struct ModuleStatus {
  bool valid = false;
  uint32_t ageMs = 0;
  audiolink::StatusPayload status{};
};

/** Starts the link task (once). */
void begin();

/** Stops reading the bridge, e.g. while the raw Link test owns it. */
void setSuspended(bool suspended);

/** HELLO received in this USB session, so commands and audio can flow. */
bool ready();
audiolink::HelloPayload hello();
/** Increments every time a new session starts (bridge reconnected or module rebooted). */
uint32_t sessionCounter();

ModuleStatus status();
std::vector<BtDevice> btDevices();
void clearBtDevices();

/** Reliable host frame. False when the link is not ready or the window stayed full. */
bool send(audiolink::Type type, const void* payload, size_t length, uint32_t timeoutMs);

/** AUDIO_DATA for streamId (length <= MAX_PAYLOAD - 1). Waits for window room and credit. */
bool sendAudio(uint8_t streamId, const uint8_t* data, size_t length, uint32_t timeoutMs);

struct Counters {
  uint32_t framesSent = 0;
  uint32_t resends = 0;
  uint32_t badFrames = 0;
  uint32_t audioBytes = 0;
};
Counters counters();

}  // namespace AudioLink

#endif  // INX_HAS_AUDIO_MODULE
