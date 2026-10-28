#pragma once

/**
 * @file Bq25896.h
 * @brief Minimal BQ25896 charger access for the USB-C OTG boost (LilyGo T5 S3 audio module).
 *
 * Only what the external audio module needs: read the VBUS/charge/fault status, run a one-shot
 * ADC conversion, and switch the OTG boost that drives 5 V out of the USB-C port. BatteryMonitor
 * keeps doing the read-only charge status for the battery icon.
 *
 * Board wiring (T5 E-paper S3 Pro V1.0 schematic, page 1): the connector's VBUS goes straight to
 * the charger's BUS pin, so the boost output is the port's VBUS. The charger's OTG pin is pulled up
 * to VSYS (R25, 10k), so the boost is controlled only by REG03 OTG_CONFIG. Both CC pins have 5.1k
 * sink pull-downs; nothing negotiates a source role.
 *
 * Register defaults matter here: OTG_CONFIG resets to 1, and the I2C watchdog (40 s by default)
 * restores every register to its default when it expires. A write that turns the boost off
 * therefore also disables the watchdog, or the boost would come back on 40 s later.
 */

#if INX_HAS_AUDIO_MODULE

#include <cstdint>

namespace Bq25896 {

/** REG0B VBUS_STAT values. */
enum class VbusStat : uint8_t {
  NoInput = 0,
  UsbHostSdp = 1,
  Adapter = 2,
  Otg = 7,
};

struct Status {
  uint8_t reg03 = 0;
  uint8_t reg07 = 0;
  uint8_t reg0b = 0;
  uint8_t reg0c = 0;  ///< Fault register as latched since the previous read
  uint8_t reg11 = 0;
  /** REG03 OTG_CONFIG: boost requested. */
  bool otgConfig = false;
  /** REG0B VBUS_STAT[7:5]. */
  uint8_t vbusStat = 0;
  /** REG0B PG_STAT: a valid input source is present. */
  bool powerGood = false;
  /** REG11 VBUS_GD. */
  bool vbusGood = false;
  /** REG0C BOOST_FAULT: VBUS overloaded or overvoltage while boosting. */
  bool boostFault = false;
  /** REG0C WATCHDOG_FAULT: the I2C watchdog expired and registers went back to defaults. */
  bool watchdogFault = false;
  /** VBUS from the last ADC conversion, mV. 0 until a conversion has run. */
  uint16_t vbusMv = 0;
};

/** True when the charger answers on the board's I2C bus. */
bool present();

/** Reads the status registers. Reading REG0C clears its latched faults. */
bool readStatus(Status& out);

/** Starts a one-shot ADC conversion. VBUS/VSYS/VBAT are valid about 1 s later. */
bool startAdcConversion();

/** True when something other than the T5's own boost is driving VBUS (charger, PC, powered module). */
bool externalVbusPresent(const Status& status);

/** Sets REG07 WATCHDOG to 00 so register writes are not reverted to defaults. */
bool disableWatchdog();

/** Sets REG03 OTG_CONFIG (and disables the watchdog first). Does not check VBUS; callers do. */
bool setOtgConfig(bool enabled);

/** Short label for a VBUS_STAT value, for logs and the diagnostics screen. */
const char* vbusStatName(uint8_t vbusStat);

}  // namespace Bq25896

#endif  // INX_HAS_AUDIO_MODULE
