/**
 * @file AudioModuleActivity.cpp
 * @brief Definitions for AudioModuleActivity.
 */

#include "AudioModuleActivity.h"

#if INX_HAS_AUDIO_MODULE

#include <GfxRenderer.h>

#include <cstdio>
#include <string>

#include "activity/page/SubPage.h"
#include "system/AudioModule.h"
#include "system/Fonts.h"
#include "system/MappedInputManager.h"
#include "system/UiLayout.h"

namespace {

constexpr int ACTION_COUNT = 3;
constexpr int ROW_HEIGHT = UiLayout::LIST_ITEM_HEIGHT;
constexpr unsigned long MIN_REDRAW_MS = 1000;

const char* linkStateName(const UsbSerialHost::State state) {
  switch (state) {
    case UsbSerialHost::State::Stopped:
      return "off";
    case UsbSerialHost::State::Waiting:
      return "waiting for module";
    case UsbSerialHost::State::Connected:
      return "connected";
    case UsbSerialHost::State::Failed:
      return "failed (see log)";
  }
  return "?";
}

}  // namespace

void AudioModuleActivity::onEnter() {
  AudioModule::snapshot();
  updateRequired = true;
}

void AudioModuleActivity::loop() {
  if (SubPage::closeInput(renderer, mappedInput, onClose)) return;

  if (mappedInput.hasTouch()) {
    float tapNx = 0.0f;
    float tapNy = 0.0f;
    if (mappedInput.wasTouchTapInScreen(renderer, tapNx, tapNy)) {
      const int tapY = static_cast<int>(tapNy * renderer.getScreenHeight());
      if (tapY >= actionsTop && tapY < actionsTop + ACTION_COUNT * ROW_HEIGHT) {
        selectedIndex = (tapY - actionsTop) / ROW_HEIGHT;
        runAction(selectedIndex);
        return;
      }
    }
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    runAction(selectedIndex);
    return;
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Up)) {
    selectedIndex = (selectedIndex + ACTION_COUNT - 1) % ACTION_COUNT;
    selectedVisible = true;
    updateRequired = true;
  } else if (mappedInput.wasPressed(MappedInputManager::Button::Down)) {
    selectedIndex = (selectedIndex + 1) % ACTION_COUNT;
    selectedVisible = true;
    updateRequired = true;
  }

  // Redraw on state changes, but at most once a second: traffic counters move constantly.
  const AudioModule::Snapshot snap = AudioModule::snapshot();
  const uint32_t traffic = snap.counters.rxBytes + snap.counters.txBytes + snap.counters.connects;
  const bool changed = AudioModule::changeCounter() != lastChange || traffic != lastTraffic;
  if (updateRequired || (changed && millis() - lastRenderMs >= MIN_REDRAW_MS)) {
    updateRequired = false;
    render();
  }
}

void AudioModuleActivity::runAction(const int index) {
  switch (index) {
    case 0: {
      const bool on = !AudioModule::isPowerOn();
      const AudioModule::PowerResult result = AudioModule::setPower(on);
      if (result != AudioModule::PowerResult::Ok) {
        AudioModule::logf("Power: %s", AudioModule::powerResultText(result));
      }
      break;
    }
    case 1:
      if (!AudioModule::isLinkEnabled()) {
        AudioModule::logf("Link: USB console off until reboot");
      }
      AudioModule::setLinkEnabled(!AudioModule::isLinkEnabled());
      break;
    case 2:
      AudioModule::runLinkTest();
      break;
    default:
      break;
  }
  updateRequired = true;
}

void AudioModuleActivity::render() {
  const AudioModule::Snapshot snap = AudioModule::snapshot();
  lastChange = AudioModule::changeCounter();
  lastTraffic = snap.counters.rxBytes + snap.counters.txBytes + snap.counters.connects;
  lastRenderMs = millis();

  renderer.clearScreen();
  actionsTop = SubPage::header(renderer, "Audio Module") + 10;
  const int screenWidth = renderer.getScreenWidth();
  const int screenHeight = renderer.getScreenHeight();
  const int font = systemFontId();
  constexpr int smallFont = MONTSERRAT_8_FONT_ID;

  const char* labels[ACTION_COUNT] = {"Module power", "USB link", "Link test"};
  const char* values[ACTION_COUNT] = {snap.powerOn ? "On" : "Off", snap.linkOn ? "On" : "Off", "›"};
  for (int i = 0; i < ACTION_COUNT; ++i) {
    const int rowY = actionsTop + i * ROW_HEIGHT;
    const bool selected = selectedVisible && i == selectedIndex;
    if (selected) {
      renderer.rectangle.fill(0, rowY, screenWidth, ROW_HEIGHT, static_cast<int>(GfxRenderer::FillTone::Ink));
    }
    const int textY = rowY + (ROW_HEIGHT - renderer.text.getLineHeight(font)) / 2;
    renderer.text.render(font, 20, textY, labels[i], !selected);
    const int valueWidth = renderer.text.getWidth(font, values[i]);
    renderer.text.render(font, screenWidth - valueWidth - 30, textY, values[i], !selected);
    renderer.line.render(0, rowY + ROW_HEIGHT - 1, screenWidth, rowY + ROW_HEIGHT - 1, true,
                         LineRender::Style::Dotted);
  }

  char charger[96];
  if (snap.chargerOk) {
    snprintf(charger, sizeof(charger), "%s, %.1f V%s%s", Bq25896::vbusStatName(snap.charger.vbusStat),
             snap.charger.vbusMv / 1000.0, snap.charger.otgConfig ? ", OTG_CONFIG=1" : "",
             snap.charger.boostFault ? ", BOOST FAULT" : "");
  } else {
    snprintf(charger, sizeof(charger), "not read yet");
  }

  char battery[96];
  if (snap.gauge.ok) {
    snprintf(battery, sizeof(battery), "%.2f V, %d mA avg, full %u / design %u mAh", snap.gauge.voltageMv / 1000.0,
             snap.gauge.averageCurrentMa, snap.gauge.fullChargeMah, snap.gauge.designMah);
  } else {
    snprintf(battery, sizeof(battery), "not read yet");
  }

  char device[96];
  if (snap.linkState == UsbSerialHost::State::Connected) {
    snprintf(device, sizeof(device), "%04X:%04X %s %s", snap.device.vid, snap.device.pid, snap.device.driver,
             snap.device.product);
  } else {
    snprintf(device, sizeof(device), "%s", linkStateName(snap.linkState));
  }

  char traffic[96];
  snprintf(traffic, sizeof(traffic), "rx %lu  tx %lu  dropped %lu  line err %lu  plugs %lu/%lu",
           static_cast<unsigned long>(snap.counters.rxBytes), static_cast<unsigned long>(snap.counters.txBytes),
           static_cast<unsigned long>(snap.counters.rxDropped), static_cast<unsigned long>(snap.counters.lineErrors),
           static_cast<unsigned long>(snap.counters.connects), static_cast<unsigned long>(snap.counters.disconnects));

  char toModule[96];
  char fromModule[96];
  if (snap.link.ran) {
    snprintf(toModule, sizeof(toModule), "%lu/%lu bytes, %lu bad, %lu ms%s",
             static_cast<unsigned long>(snap.link.toModuleGot), static_cast<unsigned long>(snap.link.toModuleSent),
             static_cast<unsigned long>(snap.link.toModuleBad), static_cast<unsigned long>(snap.link.toModuleMs),
             snap.link.toModuleReplied ? "" : " (no reply)");
    snprintf(fromModule, sizeof(fromModule), "paced %lu/%lu %s, unpaced %lu/%lu %s",
             static_cast<unsigned long>(snap.link.pacedGot), static_cast<unsigned long>(snap.link.fromModuleSent),
             snap.link.pacedExact ? "exact" : "damaged", static_cast<unsigned long>(snap.link.burstGot),
             static_cast<unsigned long>(snap.link.fromModuleSent), snap.link.burstExact ? "exact" : "damaged");
  } else {
    snprintf(toModule, sizeof(toModule), "not run");
    snprintf(fromModule, sizeof(fromModule), "not run");
  }

  struct Row {
    const char* label;
    const char* value;
  };
  const Row rows[] = {{"CHARGER / VBUS", charger}, {"BATTERY", battery},
                      {"BRIDGE", device},           {"TRAFFIC", traffic},
                      {"T5 -> MODULE", toModule},   {"MODULE -> T5", fromModule}};

  const int valueMaxW = screenWidth - 40;
  int y = actionsTop + ACTION_COUNT * ROW_HEIGHT + 16;
  for (const Row& row : rows) {
    renderer.text.render(smallFont, 20, y, row.label, true, EpdFontFamily::BOLD);
    y += renderer.text.getLineHeight(smallFont) + 2;
    const std::string clipped = renderer.text.truncate(font, row.value, valueMaxW);
    renderer.text.render(font, 20, y, clipped.c_str(), true, EpdFontFamily::REGULAR);
    y += renderer.text.getLineHeight(font) + 10;
  }

  renderer.text.render(smallFont, 20, y, "LOG", true, EpdFontFamily::BOLD);
  const int logLineHeight = renderer.text.getLineHeight(smallFont) + 2;
  y += logLineHeight;
  const int bottom = screenHeight - 10;
  const size_t fit = y < bottom ? static_cast<size_t>((bottom - y) / logLineHeight) : 0;
  for (const std::string& line : AudioModule::recentLog(fit)) {
    const std::string clipped = renderer.text.truncate(smallFont, line.c_str(), valueMaxW);
    renderer.text.render(smallFont, 20, y, clipped.c_str(), true, EpdFontFamily::REGULAR);
    y += logLineHeight;
  }

  renderer.displayBuffer();
}

#endif  // INX_HAS_AUDIO_MODULE
