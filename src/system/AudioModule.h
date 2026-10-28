#pragma once

/**
 * @file AudioModule.h
 * @brief External USB-C audio module (M5Stack ATOM Lite) power and link control.
 *
 * One USB-C cable carries both power and data. Power comes from the BQ25896 OTG boost
 * (Bq25896.h); data is a USB-to-UART bridge on the module that the T5 talks to as a USB host
 * (UsbSerialHost.h). Both are off at boot and switched from the Audio Module screen.
 *
 * Power rules:
 * - The boost is never turned on while something else drives VBUS (charger, PC, or a module
 *   with its own battery). The link works without the boost in that case.
 * - The boost goes off when the link is on but no bridge has been connected for NO_DEVICE_POWER_OFF_MS
 *   (LINK_OFF_POWER_OFF_MS while the link is off, since no module can be detected then), on a boost
 *   fault, and before deep sleep.
 *
 * All calls are from the main loop task except logf(), which any task may call.
 */

#if INX_HAS_AUDIO_MODULE

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "Bq25896.h"
#include "UsbSerialHost.h"

#ifndef INX_AUDIO_MODULE_BAUD
#define INX_AUDIO_MODULE_BAUD 500000
#endif

namespace AudioModule {

constexpr uint32_t LINK_BAUD = INX_AUDIO_MODULE_BAUD;
constexpr uint32_t NO_DEVICE_POWER_OFF_MS = 20000;
constexpr uint32_t LINK_OFF_POWER_OFF_MS = 5 * 60 * 1000;

enum class PowerResult : uint8_t { Ok, ExternalVbus, NoCharger, I2cError, BoostFault };

/**
 * One-direction link test against the module's link-diagnostic firmware (audio-module env
 * atom_lite). The ATOM Lite's FTDI-emulating bridge is solid host -> module but drops module ->
 * host data unless it is trickled, so each direction is measured separately.
 */
struct LinkTestResult {
  bool ran = false;
  /** Host -> module: bytes the module received, how many were wrong, and how long it took. */
  uint32_t toModuleSent = 0;
  uint32_t toModuleGot = 0;
  uint32_t toModuleBad = 0;
  uint32_t toModuleMs = 0;
  bool toModuleReplied = false;
  /** Module -> host, trickled (16-byte bursts every 4 ms) and unpaced (one continuous burst). */
  uint32_t fromModuleSent = 0;
  uint32_t pacedGot = 0;
  bool pacedExact = false;
  uint32_t burstGot = 0;
  bool burstExact = false;
};

/** BQ27220 readings, to see what the module costs in battery. */
struct GaugeReading {
  bool ok = false;
  uint16_t voltageMv = 0;
  int16_t averageCurrentMa = 0;  ///< Negative while discharging
  uint16_t fullChargeMah = 0;    ///< Learned capacity
  uint16_t designMah = 0;        ///< As programmed into the gauge, not necessarily the cell's rating
};

struct Snapshot {
  bool chargerOk = false;
  Bq25896::Status charger;
  bool externalVbus = false;
  bool powerOn = false;
  bool linkOn = false;
  UsbSerialHost::State linkState = UsbSerialHost::State::Stopped;
  UsbSerialHost::DeviceInfo device;
  UsbSerialHost::Counters counters;
  LinkTestResult link;
  GaugeReading gauge;
};

/** Boot: logs the charger state found and puts the boost in a known (off) state. */
void begin();

/** Main-loop housekeeping: charger polling, boost fault handling, auto power-off. */
void service();

/** Before deep sleep: stops the link and turns the boost off. */
void shutdown();

PowerResult setPower(bool on);
bool isPowerOn();

/** Starts or stops the USB host link. Starting it takes the USB port away from the console. */
bool setLinkEnabled(bool on);
bool isLinkEnabled();

/** Measures both link directions (needs the module's link-diagnostic firmware). Blocks ~3 s. */
LinkTestResult runLinkTest();

Snapshot snapshot();

const char* powerResultText(PowerResult result);

/** Appends a line to the module log (shown on screen) and to INX_SERIAL. Any task. */
void logf(const char* format, ...) __attribute__((format(printf, 1, 2)));

/** The newest log lines, oldest first. */
std::vector<std::string> recentLog(size_t maxLines);

/** Increments whenever the snapshot or the log changes, so the screen knows when to redraw. */
uint32_t changeCounter();

}  // namespace AudioModule

#endif  // INX_HAS_AUDIO_MODULE
