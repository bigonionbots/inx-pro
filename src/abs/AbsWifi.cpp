/**
 * @file AbsWifi.cpp
 * @brief Definitions for the on-demand Wi-Fi connection.
 */

#include "AbsWifi.h"

#if INX_HAS_AUDIO_MODULE

#include <Arduino.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <utility>
#include <vector>

#include "state/NetworkCredential.h"
#include "system/AudioModule.h"
#include "util/SdIoMutex.h"

namespace AbsWifi {

namespace {

constexpr uint32_t CONNECT_TIMEOUT_MS = 15000;

SemaphoreHandle_t lock = nullptr;
std::vector<std::pair<std::string, std::string>> networks;  // ssid, password
int users = 0;
bool turnedOn = false;

void ensureLock() {
  if (!lock) lock = xSemaphoreCreateRecursiveMutex();
}

/** Why a join attempt ended without a connection, from the station status it left. */
const char* joinFailure(const wl_status_t status) {
  switch (status) {
    case WL_NO_SSID_AVAIL:
      return "not found";  // not on the air: out of range, hotspot asleep, or 5/6 GHz only
    case WL_CONNECT_FAILED:
      return "refused";  // found, but would not let the T5 join (wrong password?)
    case WL_CONNECTION_LOST:
      return "connection lost";
    default:
      return "no answer";  // found, but the join timed out
  }
}

/** Turns Wi-Fi off, saying why in the module log (the console is gone while the module runs). */
void turnOff(const char* why) {
  WiFi.disconnect(false);
  WiFi.mode(WIFI_OFF);
  AudioModule::logf("Wi-Fi: off (%s)", why);
}

}  // namespace

void prepare() {
  ensureLock();
  {
    SdIoMutex::Lock sdLock;
    WIFI_STORE.loadFromFile();
  }
  xSemaphoreTakeRecursive(lock, portMAX_DELAY);
  networks.clear();
  for (const auto& credential : WIFI_STORE.getCredentials()) {
    if (!credential.ssid.empty()) networks.emplace_back(credential.ssid, credential.password);
  }
  xSemaphoreGiveRecursive(lock);
}

bool hasSavedNetworks() {
  ensureLock();
  xSemaphoreTakeRecursive(lock, portMAX_DELAY);
  const bool any = !networks.empty();
  xSemaphoreGiveRecursive(lock);
  return any;
}

bool connected() { return WiFi.status() == WL_CONNECTED; }

/**
 * The T5 runs its own hotspot (HotspotActivity, AP mode). Joining a network would switch the radio
 * to station mode and take the hotspot down under whoever is connected to it, and a failed join
 * turned Wi-Fi off altogether: leave the radio alone while it lasts.
 */
bool hotspotActive() { return (WiFi.getMode() & WIFI_MODE_AP) != 0; }

bool acquire(std::string& error) {
  ensureLock();
  xSemaphoreTakeRecursive(lock, portMAX_DELAY);
  if (WiFi.status() == WL_CONNECTED) {
    ++users;
    xSemaphoreGiveRecursive(lock);
    return true;
  }
  if (hotspotActive()) {
    xSemaphoreGiveRecursive(lock);
    error = "Wi-Fi is in hotspot mode";
    return false;
  }
  if (networks.empty()) {
    xSemaphoreGiveRecursive(lock);
    error = "No saved Wi-Fi network";
    return false;
  }
  if (!WiFi.mode(WIFI_STA)) {
    // The driver needs internal RAM for its buffers; it is what runs out first.
    WiFi.mode(WIFI_OFF);
    xSemaphoreGiveRecursive(lock);
    error = "Wi-Fi could not start (low memory?)";
    AudioModule::logf("Wi-Fi: could not start (internal RAM %u bytes free)",
                      static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)));
    return false;
  }
  std::string tried;  // "HighTail not found, Home refused"
  bool anyNotFound = false;
  for (const auto& network : networks) {
    AudioModule::logf("Wi-Fi: joining %s", network.first.c_str());
    WiFi.begin(network.first.c_str(), network.second.c_str());
    const uint32_t started = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - started < CONNECT_TIMEOUT_MS) {
      vTaskDelay(pdMS_TO_TICKS(250));
    }
    const wl_status_t status = WiFi.status();
    if (status == WL_CONNECTED) {
      turnedOn = true;
      ++users;
      AudioModule::logf("Wi-Fi: joined %s (%d dBm, %s)", network.first.c_str(), WiFi.RSSI(),
                        WiFi.localIP().toString().c_str());
      xSemaphoreGiveRecursive(lock);
      return true;
    }
    const char* why = joinFailure(status);
    anyNotFound = anyNotFound || status == WL_NO_SSID_AVAIL;
    AudioModule::logf("Wi-Fi: %s %s after %lu ms (status %d)", network.first.c_str(), why,
                      static_cast<unsigned long>(millis() - started), static_cast<int>(status));
    tried += (tried.empty() ? "" : ", ") + network.first + " " + why;
    WiFi.disconnect(false);
    vTaskDelay(pdMS_TO_TICKS(100));
  }
  turnOff("could not join");
  xSemaphoreGiveRecursive(lock);
  error = "Could not join Wi-Fi: " + tried + (anyNotFound ? " (hotspot asleep, or 5 GHz only?)" : "");
  return false;
}

void adopt() {
  ensureLock();
  xSemaphoreTakeRecursive(lock, portMAX_DELAY);
  if (WiFi.status() == WL_CONNECTED) turnedOn = true;
  xSemaphoreGiveRecursive(lock);
}

void offIfUnused() {
  ensureLock();
  xSemaphoreTakeRecursive(lock, portMAX_DELAY);
  if (users == 0 && !hotspotActive()) {
    if (WiFi.getMode() != WIFI_OFF) turnOff("screen closed");
    turnedOn = false;
  }
  xSemaphoreGiveRecursive(lock);
}

void release() {
  ensureLock();
  xSemaphoreTakeRecursive(lock, portMAX_DELAY);
  if (users > 0) --users;
  if (users == 0 && turnedOn) {
    // Not under a hotspot started since: that is its screen's to turn off.
    if (!hotspotActive()) turnOff("not needed");
    turnedOn = false;
  }
  xSemaphoreGiveRecursive(lock);
}

}  // namespace AbsWifi

#endif  // INX_HAS_AUDIO_MODULE
