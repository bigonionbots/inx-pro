#pragma once

/**
 * @file Link.h
 * @brief Module end of the T5 link (shared/audio_link/AudioLinkProtocol.h) over UART0.
 *
 * Reliable host frames are accepted strictly in sequence. PING is the exception: it is accepted
 * with any sequence number and resynchronises the expected one, so a T5 that reconnects (or a
 * module that rebooted and sent HELLO) never deadlocks on stale sequence numbers.
 */

#include <cstdint>

namespace hostlink {

bool begin();

/** Sends a LOG frame to the T5's module log. Any task. */
void log(const char* format, ...) __attribute__((format(printf, 1, 2)));

/** Sends one BT_DEVICE frame. Any task (called from the Bluetooth task while scanning). */
void sendBtDevice(const uint8_t address[6], int8_t rssi, const char* name);

}  // namespace hostlink
