#pragma once

/**
 * @file ScreenWifi.h
 * @brief Turning Wi-Fi off again when a screen that connected it closes.
 *
 * Screens that open the Wi-Fi picker to get online (Manage via WiFi, the Store's font, dictionary,
 * plugin and language managers) call off() on exit if their picker connected. Wi-Fi left on costs
 * battery and the driver's internal RAM, which TLS later needs (KOReader Sync).
 */

#include <WiFi.h>

#if INX_HAS_AUDIO_MODULE
#include "abs/AbsWifi.h"
#endif

namespace ScreenWifi {

/** Turns Wi-Fi off, unless the audio player or a hotspot still uses it (audio module builds). */
inline void off() {
#if INX_HAS_AUDIO_MODULE
  AbsWifi::offIfUnused();
#else
  WiFi.disconnect(false);
  WiFi.mode(WIFI_OFF);
#endif
}

}  // namespace ScreenWifi
