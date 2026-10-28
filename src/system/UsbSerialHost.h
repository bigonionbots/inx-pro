#pragma once

/**
 * @file UsbSerialHost.h
 * @brief USB host link to the audio module's USB-to-UART bridge (LilyGo T5 S3 only).
 *
 * The T5's USB-C port is the ESP32-S3's native USB. start() installs the ESP-IDF USB Host Library
 * plus the CDC-ACM class driver and the FTDI / CP210x / CH34x VCP drivers (vendored under
 * third_party/usb_host), which moves the shared USB PHY away from USB-Serial-JTAG: the USB console
 * and esptool reset stop working until the device reboots. stop() tears the stack down again.
 *
 * A background task opens whichever bridge enumerates, picking the driver by vendor ID (CH9102 and
 * other CDC-compliant bridges use the generic CDC-ACM driver), sets 8N1 at the requested baud rate,
 * and holds DTR and RTS deasserted so the ESP32 auto-reset circuit on the module neither resets it
 * nor puts it in its bootloader. Received bytes land in a stream buffer for read(). Unplugging and
 * replugging is handled: the task closes the device and waits for the next one.
 */

#if INX_HAS_AUDIO_MODULE

#include <cstddef>
#include <cstdint>

namespace UsbSerialHost {

enum class State : uint8_t {
  Stopped,    ///< Host stack not installed; the PHY belongs to USB-Serial-JTAG
  Waiting,    ///< Host stack running, no bridge open
  Connected,  ///< A bridge is open and configured
  Failed,     ///< start() failed; see the module log
};

struct DeviceInfo {
  uint16_t vid = 0;
  uint16_t pid = 0;
  const char* driver = "";  ///< "FTDI", "CP210x", "CH34x" or "CDC-ACM"
  char product[40] = {};
};

struct Counters {
  uint32_t rxBytes = 0;
  uint32_t txBytes = 0;
  uint32_t rxDropped = 0;  ///< Bytes lost because the receive buffer was full
  uint32_t connects = 0;
  uint32_t disconnects = 0;
  uint32_t lineErrors = 0;  ///< Overrun/framing/parity reports from the bridge (FTDI)
};

/** Installs the host stack and starts the connection task. No-op if already running. */
bool start(uint32_t baud);

/** Closes the bridge and uninstalls the host stack. Blocks until the stack is down. */
void stop();

State state();
bool isConnected();
DeviceInfo device();
Counters counters();

/** Blocking write. Returns the bytes written: all or nothing, 0 when not connected or on timeout. */
size_t write(const uint8_t* data, size_t length, uint32_t timeoutMs);

/** Reads up to length received bytes, waiting up to timeoutMs for the first one. */
size_t read(uint8_t* data, size_t length, uint32_t timeoutMs);

/** Drops everything waiting in the receive buffer. */
void clearReceived();

}  // namespace UsbSerialHost

#endif  // INX_HAS_AUDIO_MODULE
