#pragma once

/**
 * @file AudioModuleActivity.h
 * @brief Public interface and types for AudioModuleActivity.
 */

#if INX_HAS_AUDIO_MODULE

#include <functional>

#include "activity/Activity.h"

/**
 * Audio module bench screen, reached from Device Management -> Audio Module. Switches the USB-C
 * OTG boost and the USB host link, runs an echo test against the module, and shows the charger
 * status, the connected bridge (VID:PID, driver), traffic counters, and the module log. While the
 * USB link is on the USB console is gone, so this screen is where the diagnostics go.
 */
class AudioModuleActivity final : public Activity {
 public:
  explicit AudioModuleActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                               const std::function<void()>& onClose)
      : Activity("AudioModule", renderer, mappedInput), onClose(onClose) {}

  void onEnter() override;
  void loop() override;

 private:
  const std::function<void()> onClose;
  int selectedIndex = 0;
  bool selectedVisible = false;
  bool updateRequired = false;
  int actionsTop = 0;
  uint32_t lastChange = 0;
  uint32_t lastTraffic = 0;
  unsigned long lastRenderMs = 0;

  void runAction(int index);
  void render();
};

#endif  // INX_HAS_AUDIO_MODULE
