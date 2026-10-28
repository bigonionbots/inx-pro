/**
 * @file Bq25896.cpp
 * @brief Definitions for the BQ25896 OTG helpers.
 */

#include "Bq25896.h"

#if INX_HAS_AUDIO_MODULE

#include <Arduino.h>
#include <BoardConfig.h>
#include <Wire.h>

namespace Bq25896 {

namespace {

constexpr uint8_t REG02 = 0x02;  // CONV_START bit 7
constexpr uint8_t REG03 = 0x03;  // OTG_CONFIG bit 5
constexpr uint8_t REG07 = 0x07;  // WATCHDOG bits 5:4
constexpr uint8_t REG0B = 0x0B;  // VBUS_STAT 7:5, CHRG_STAT 4:3, PG_STAT 2
constexpr uint8_t REG0C = 0x0C;  // WATCHDOG_FAULT 7, BOOST_FAULT 6
constexpr uint8_t REG11 = 0x11;  // VBUS_GD 7, VBUSV 6:0

constexpr uint8_t CONV_START = 0x80;
constexpr uint8_t OTG_CONFIG = 0x20;
constexpr uint8_t WATCHDOG_MASK = 0x30;

uint8_t address() { return BoardConfig::ACTIVE.batteryGauge.chargerAddr; }

// BoardT5S3::begin() has already started Wire on the shared SDA39/SCL40 bus. Wire serialises
// transactions internally, so these calls are safe next to the GT911 and PCA9535 traffic.
bool readReg(const uint8_t reg, uint8_t& value) {
  Wire.beginTransmission(address());
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(address(), static_cast<uint8_t>(1)) != 1) return false;
  value = static_cast<uint8_t>(Wire.read());
  return true;
}

bool writeReg(const uint8_t reg, const uint8_t value) {
  Wire.beginTransmission(address());
  Wire.write(reg);
  Wire.write(value);
  return Wire.endTransmission() == 0;
}

bool updateReg(const uint8_t reg, const uint8_t mask, const uint8_t bits) {
  uint8_t value = 0;
  if (!readReg(reg, value)) return false;
  const uint8_t next = static_cast<uint8_t>((value & ~mask) | (bits & mask));
  if (next == value) return true;
  return writeReg(reg, next);
}

}  // namespace

bool present() {
  if (address() == 0) return false;
  Wire.beginTransmission(address());
  return Wire.endTransmission() == 0;
}

bool readStatus(Status& out) {
  if (address() == 0) return false;
  if (!readReg(REG03, out.reg03) || !readReg(REG07, out.reg07) || !readReg(REG0B, out.reg0b) ||
      !readReg(REG0C, out.reg0c) || !readReg(REG11, out.reg11)) {
    return false;
  }
  out.otgConfig = (out.reg03 & OTG_CONFIG) != 0;
  out.vbusStat = static_cast<uint8_t>(out.reg0b >> 5);
  out.powerGood = (out.reg0b & 0x04) != 0;
  out.vbusGood = (out.reg11 & 0x80) != 0;
  out.boostFault = (out.reg0c & 0x40) != 0;
  out.watchdogFault = (out.reg0c & 0x80) != 0;
  const uint8_t vbusCode = out.reg11 & 0x7F;
  out.vbusMv = vbusCode == 0 ? 0 : static_cast<uint16_t>(2600 + 100 * vbusCode);
  return true;
}

bool startAdcConversion() { return updateReg(REG02, CONV_START, CONV_START); }

bool externalVbusPresent(const Status& status) {
  if (status.vbusStat == static_cast<uint8_t>(VbusStat::Otg)) return false;
  return status.vbusStat != static_cast<uint8_t>(VbusStat::NoInput) || status.powerGood || status.vbusGood;
}

bool disableWatchdog() { return updateReg(REG07, WATCHDOG_MASK, 0); }

bool setOtgConfig(const bool enabled) {
  if (!disableWatchdog()) return false;
  return updateReg(REG03, OTG_CONFIG, enabled ? OTG_CONFIG : 0);
}

const char* vbusStatName(const uint8_t vbusStat) {
  switch (vbusStat) {
    case 0:
      return "no input";
    case 1:
      return "USB host (SDP)";
    case 2:
      return "adapter";
    case 7:
      return "OTG boost";
    default:
      return "other input";
  }
}

}  // namespace Bq25896

#endif  // INX_HAS_AUDIO_MODULE
