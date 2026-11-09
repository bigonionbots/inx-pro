/**
 * @file AbsWifi.cpp
 * @brief Definitions for the on-demand Wi-Fi connection.
 */

#include "AbsWifi.h"

#if INX_HAS_AUDIO_MODULE

#include <Arduino.h>
#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <utility>
#include <vector>

#include "state/NetworkCredential.h"
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
  WiFi.mode(WIFI_STA);
  for (const auto& network : networks) {
    WiFi.begin(network.first.c_str(), network.second.c_str());
    const uint32_t started = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - started < CONNECT_TIMEOUT_MS) {
      vTaskDelay(pdMS_TO_TICKS(250));
    }
    if (WiFi.status() == WL_CONNECTED) {
      turnedOn = true;
      ++users;
      xSemaphoreGiveRecursive(lock);
      return true;
    }
    WiFi.disconnect(false);
    vTaskDelay(pdMS_TO_TICKS(100));
  }
  std::string tried;
  for (const auto& network : networks) tried += (tried.empty() ? "" : ", ") + network.first;
  WiFi.mode(WIFI_OFF);
  xSemaphoreGiveRecursive(lock);
  error = "Could not join Wi-Fi (" + tried + ")";
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
    WiFi.disconnect(false);
    WiFi.mode(WIFI_OFF);
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
    if (!hotspotActive()) {
      WiFi.disconnect(false);
      WiFi.mode(WIFI_OFF);
    }
    turnedOn = false;
  }
  xSemaphoreGiveRecursive(lock);
}

}  // namespace AbsWifi

#endif  // INX_HAS_AUDIO_MODULE
