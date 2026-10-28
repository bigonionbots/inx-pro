/**
 * @file AudioModule.cpp
 * @brief Definitions for the external audio module power and link control.
 */

#include "AudioModule.h"

#if INX_HAS_AUDIO_MODULE

#include "AudioLink.h"

#include <Arduino.h>
#include <BoardConfig.h>
#include <Wire.h>

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <deque>
#include <cstring>
#include <mutex>

namespace AudioModule {

namespace {

constexpr uint32_t POLL_INTERVAL_MS = 1000;
constexpr uint32_t SCREEN_POLL_WINDOW_MS = 3000;
constexpr uint32_t BOOST_SETTLE_MS = 50;
constexpr size_t LOG_LINES = 24;
constexpr size_t LINK_CHUNK_BYTES = 256;
constexpr uint32_t LINK_WRITE_TIMEOUT_MS = 500;
constexpr uint32_t LINK_TO_MODULE_BYTES = 8192;
constexpr uint32_t LINK_FROM_MODULE_BYTES = 4096;
constexpr uint16_t LINK_PACED_BURST = 16;
constexpr uint16_t LINK_PACED_GAP_US = 4000;
constexpr uint32_t LINK_REPLY_TIMEOUT_MS = 3000;
constexpr uint32_t LINK_IDLE_TIMEOUT_MS = 800;

std::mutex logMutex;
std::deque<std::string> logLines;
std::atomic<uint32_t> changes{0};

bool chargerOk = false;
Bq25896::Status charger;
bool powerOn = false;
bool linkOn = false;
uint32_t lastPollMs = 0;
uint32_t lastSnapshotMs = 0;
uint32_t noDeviceSinceMs = 0;
LinkTestResult lastLink;
GaugeReading gauge;

void markChanged() { ++changes; }

// BQ27220 standard commands (16-bit little endian), for the module's power budget.
constexpr uint8_t GAUGE_VOLTAGE = 0x08;
constexpr uint8_t GAUGE_FULL_CHARGE_CAPACITY = 0x12;
constexpr uint8_t GAUGE_AVERAGE_CURRENT = 0x14;
constexpr uint8_t GAUGE_DESIGN_CAPACITY = 0x3C;

bool readGaugeWord(const uint8_t command, uint16_t& value) {
  const uint8_t address = BoardConfig::ACTIVE.batteryGauge.gaugeAddr;
  if (address == 0) return false;
  Wire.beginTransmission(address);
  Wire.write(command);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(address, static_cast<uint8_t>(2)) != 2) return false;
  const uint8_t lo = static_cast<uint8_t>(Wire.read());
  const uint8_t hi = static_cast<uint8_t>(Wire.read());
  value = static_cast<uint16_t>(lo | (hi << 8));
  return true;
}

void pollGauge() {
  GaugeReading next;
  uint16_t voltage = 0;
  uint16_t current = 0;
  next.ok = readGaugeWord(GAUGE_VOLTAGE, voltage) && readGaugeWord(GAUGE_AVERAGE_CURRENT, current) &&
            readGaugeWord(GAUGE_FULL_CHARGE_CAPACITY, next.fullChargeMah) &&
            readGaugeWord(GAUGE_DESIGN_CAPACITY, next.designMah);
  next.voltageMv = voltage;
  next.averageCurrentMa = static_cast<int16_t>(current);
  if (next.ok != gauge.ok || next.averageCurrentMa != gauge.averageCurrentMa) markChanged();
  gauge = next;
}

bool pollCharger() {
  Bq25896::Status status;
  const bool ok = Bq25896::readStatus(status);
  if (ok != chargerOk || status.reg03 != charger.reg03 || status.reg0b != charger.reg0b ||
      status.reg0c != charger.reg0c || status.reg11 != charger.reg11) {
    markChanged();
  }
  chargerOk = ok;
  if (ok) {
    charger = status;
    // Start the next VBUS measurement now so the following poll has a fresh value.
    Bq25896::startAdcConversion();
  }
  return ok;
}

void boostOff(const char* reason) {
  Bq25896::setOtgConfig(false);
  if (powerOn) logf("Power: boost off (%s)", reason);
  powerOn = false;
  noDeviceSinceMs = 0;
  markChanged();
}

// Multiplicative hash: no short period, so a few bytes pin down their position in the stream.
// audio-module's diagnostic firmware uses the same pattern.
uint8_t linkPatternByte(const uint32_t index) { return static_cast<uint8_t>((index * 2654435761u) >> 24); }

}  // namespace

void begin() {
  if (!Bq25896::present()) {
    logf("Charger: BQ25896 not found");
    return;
  }
  pollCharger();
  logf("Charger at boot: REG03=%02X REG07=%02X REG0B=%02X OTG_CONFIG=%d VBUS=%s", charger.reg03, charger.reg07,
       charger.reg0b, charger.otgConfig ? 1 : 0, Bq25896::vbusStatName(charger.vbusStat));
  // OTG_CONFIG resets to 1, so the boost may already be driving the port. Start from off.
  if (!Bq25896::setOtgConfig(false)) logf("Charger: could not clear OTG_CONFIG");
  pollCharger();
}

void service() {
  const uint32_t now = millis();
  const bool screenWatching = lastSnapshotMs != 0 && now - lastSnapshotMs < SCREEN_POLL_WINDOW_MS;
  if (!powerOn && !linkOn && !screenWatching) return;
  if (now - lastPollMs < POLL_INTERVAL_MS) return;
  lastPollMs = now;

  pollGauge();
  if (!pollCharger()) return;

  if (powerOn) {
    if (charger.boostFault) {
      boostOff("boost fault: VBUS overloaded or driven from outside");
    } else if (Bq25896::externalVbusPresent(charger)) {
      boostOff("external power on VBUS");
    }
  }

  if (powerOn) {
    // With the link off the module cannot be seen, so only a long idle limit applies.
    const uint32_t limit = linkOn ? NO_DEVICE_POWER_OFF_MS : LINK_OFF_POWER_OFF_MS;
    if (UsbSerialHost::isConnected()) {
      noDeviceSinceMs = 0;
    } else if (noDeviceSinceMs == 0) {
      noDeviceSinceMs = now;
    } else if (now - noDeviceSinceMs >= limit) {
      boostOff(linkOn ? "no module connected" : "link never started");
    }
  }
}

void shutdown() {
  if (linkOn) setLinkEnabled(false);
  if (Bq25896::present()) Bq25896::setOtgConfig(false);
  powerOn = false;
}

PowerResult setPower(const bool on) {
  if (!on) {
    boostOff("switched off");
    return PowerResult::Ok;
  }
  if (powerOn) return PowerResult::Ok;
  if (!Bq25896::present()) return PowerResult::NoCharger;
  if (!pollCharger()) return PowerResult::I2cError;
  if (Bq25896::externalVbusPresent(charger)) {
    logf("Power: not boosting, VBUS already powered (%s)", Bq25896::vbusStatName(charger.vbusStat));
    return PowerResult::ExternalVbus;
  }
  if (!Bq25896::setOtgConfig(true)) return PowerResult::I2cError;
  delay(BOOST_SETTLE_MS);
  pollCharger();
  if (charger.boostFault) {
    Bq25896::setOtgConfig(false);
    logf("Power: boost fault right after enabling");
    return PowerResult::BoostFault;
  }
  powerOn = true;
  noDeviceSinceMs = millis();
  logf("Power: boost on, VBUS=%s", Bq25896::vbusStatName(charger.vbusStat));
  markChanged();
  return PowerResult::Ok;
}

bool isPowerOn() { return powerOn; }

bool setLinkEnabled(const bool on) {
  if (on == linkOn) return true;
  if (on) {
    linkOn = UsbSerialHost::start(LINK_BAUD);
    // Give a module that is already powered the full window to enumerate.
    if (powerOn) noDeviceSinceMs = millis();
  } else {
    UsbSerialHost::stop();
    linkOn = false;
  }
  markChanged();
  return linkOn == on;
}

bool isLinkEnabled() { return linkOn; }

namespace {

void putLe32(uint8_t* out, const uint32_t value) {
  for (int i = 0; i < 4; ++i) out[i] = static_cast<uint8_t>(value >> (8 * i));
}

// 'B' + u32 length + u16 burst + u16 gap_us: the module sends the pattern in bursts.
void receiveFromModule(const uint32_t length, const uint16_t burst, const uint16_t gapUs, uint32_t& got,
                       bool& exact) {
  UsbSerialHost::clearReceived();
  uint8_t command[9] = {'B'};
  putLe32(command + 1, length);
  command[5] = static_cast<uint8_t>(burst);
  command[6] = static_cast<uint8_t>(burst >> 8);
  command[7] = static_cast<uint8_t>(gapUs);
  command[8] = static_cast<uint8_t>(gapUs >> 8);
  got = 0;
  exact = UsbSerialHost::write(command, sizeof(command), LINK_WRITE_TIMEOUT_MS) == sizeof(command);
  uint8_t incoming[LINK_CHUNK_BYTES];
  uint32_t lastRxMs = millis();
  while (got < length && millis() - lastRxMs < LINK_IDLE_TIMEOUT_MS) {
    const size_t n = UsbSerialHost::read(incoming, sizeof(incoming), 50);
    for (size_t i = 0; i < n; ++i) {
      if (incoming[i] != linkPatternByte(got + i)) exact = false;
    }
    got += static_cast<uint32_t>(n);
    if (n > 0) lastRxMs = millis();
  }
  if (got != length) exact = false;
}

}  // namespace

LinkTestResult runLinkTest() {
  LinkTestResult result;
  if (!UsbSerialHost::isConnected()) {
    logf("Link test: no module connected");
    return result;
  }
  result.ran = true;
  // The raw test bytes are not protocol frames: keep the protocol link off the bridge meanwhile.
  AudioLink::setSuspended(true);

  // Host -> module: 'R' + u32 length, then the pattern; the module checks it and replies
  // "R <got> <bad> <ms>\n".
  UsbSerialHost::clearReceived();
  uint8_t chunk[LINK_CHUNK_BYTES];
  chunk[0] = 'R';
  putLe32(chunk + 1, LINK_TO_MODULE_BYTES);
  if (UsbSerialHost::write(chunk, 5, LINK_WRITE_TIMEOUT_MS) == 5) {
    delay(20);
    while (result.toModuleSent < LINK_TO_MODULE_BYTES) {
      const size_t n = std::min<size_t>(sizeof(chunk), LINK_TO_MODULE_BYTES - result.toModuleSent);
      for (size_t i = 0; i < n; ++i) chunk[i] = linkPatternByte(result.toModuleSent + i);
      if (UsbSerialHost::write(chunk, n, LINK_WRITE_TIMEOUT_MS) != n) break;
      result.toModuleSent += static_cast<uint32_t>(n);
    }
    char reply[48] = {};
    size_t replyLength = 0;
    const uint32_t waitStart = millis();
    while (millis() - waitStart < LINK_REPLY_TIMEOUT_MS && replyLength + 1 < sizeof(reply)) {
      uint8_t c = 0;
      if (UsbSerialHost::read(&c, 1, 50) != 1) continue;
      if (c == '\n') break;
      reply[replyLength++] = static_cast<char>(c);
    }
    unsigned long got = 0;
    unsigned long bad = 0;
    unsigned long ms = 0;
    result.toModuleReplied = sscanf(reply, "R %lu %lu %lu", &got, &bad, &ms) == 3;
    result.toModuleGot = got;
    result.toModuleBad = bad;
    result.toModuleMs = ms;
  }
  logf("Link host->module: %s got %lu/%lu bad %lu in %lu ms", result.toModuleReplied ? "" : "no reply,",
       static_cast<unsigned long>(result.toModuleGot), static_cast<unsigned long>(result.toModuleSent),
       static_cast<unsigned long>(result.toModuleBad), static_cast<unsigned long>(result.toModuleMs));

  // Module -> host, trickled and then unpaced.
  result.fromModuleSent = LINK_FROM_MODULE_BYTES;
  receiveFromModule(LINK_FROM_MODULE_BYTES, LINK_PACED_BURST, LINK_PACED_GAP_US, result.pacedGot, result.pacedExact);
  receiveFromModule(LINK_FROM_MODULE_BYTES, LINK_CHUNK_BYTES, 0, result.burstGot, result.burstExact);
  logf("Link module->host: paced %lu/%lu %s, unpaced %lu/%lu %s", static_cast<unsigned long>(result.pacedGot),
       static_cast<unsigned long>(LINK_FROM_MODULE_BYTES), result.pacedExact ? "exact" : "damaged",
       static_cast<unsigned long>(result.burstGot), static_cast<unsigned long>(LINK_FROM_MODULE_BYTES),
       result.burstExact ? "exact" : "damaged");

  AudioLink::setSuspended(false);
  lastLink = result;
  markChanged();
  return result;
}

Snapshot snapshot() {
  lastSnapshotMs = millis();
  Snapshot out;
  out.chargerOk = chargerOk;
  out.charger = charger;
  out.externalVbus = chargerOk && Bq25896::externalVbusPresent(charger);
  out.powerOn = powerOn;
  out.linkOn = linkOn;
  out.linkState = UsbSerialHost::state();
  out.device = UsbSerialHost::device();
  out.counters = UsbSerialHost::counters();
  out.link = lastLink;
  out.gauge = gauge;
  return out;
}

const char* powerResultText(const PowerResult result) {
  switch (result) {
    case PowerResult::Ok:
      return "OK";
    case PowerResult::ExternalVbus:
      return "VBUS is already powered from outside";
    case PowerResult::NoCharger:
      return "Charger not found";
    case PowerResult::I2cError:
      return "Charger I2C error";
    case PowerResult::BoostFault:
      return "Boost fault";
  }
  return "?";
}

void logf(const char* format, ...) {
  char text[160];
  va_list args;
  va_start(args, format);
  vsnprintf(text, sizeof(text), format, args);
  va_end(args);

  const uint32_t seconds = millis() / 1000;
  char line[176];
  snprintf(line, sizeof(line), "%02lu:%02lu %s", static_cast<unsigned long>(seconds / 60 % 100),
           static_cast<unsigned long>(seconds % 60), text);
  INX_SERIAL.printf("[AUDIO] %s\n", line);
  {
    std::lock_guard<std::mutex> guard(logMutex);
    logLines.emplace_back(line);
    while (logLines.size() > LOG_LINES) logLines.pop_front();
  }
  markChanged();
}

std::vector<std::string> recentLog(const size_t maxLines) {
  std::lock_guard<std::mutex> guard(logMutex);
  const size_t count = std::min(maxLines, logLines.size());
  return std::vector<std::string>(logLines.end() - static_cast<std::ptrdiff_t>(count), logLines.end());
}

uint32_t changeCounter() { return changes.load(); }

}  // namespace AudioModule

#endif  // INX_HAS_AUDIO_MODULE
