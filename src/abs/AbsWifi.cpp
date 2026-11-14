/**
 * @file AbsWifi.cpp
 * @brief Definitions for the on-demand Wi-Fi connection.
 */

#include "AbsWifi.h"

#if INX_HAS_AUDIO_MODULE

#include <Arduino.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <esp_memory_utils.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

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

/**
 * Starting, joining and stopping Wi-Fi read and write flash (NVS: the PHY calibration, the station
 * config), and flash access disables the cache that a PSRAM stack is reached through. The audio
 * feeder and sync tasks and the Audiobookshelf workers have PSRAM stacks (internal RAM is what the
 * TLS connections and the Wi-Fi driver run short of), so from a PSRAM stack these steps go to the
 * abs_wifi task, whose internal stack is made at boot while internal RAM is still in one piece
 * (a task made when needed found no 8 KB block while streaming). Handing them to the main loop
 * instead stalled it: screens wait on their workers as they close, and a worker waited on the loop.
 */
constexpr uint32_t STEP_TASK_STACK = 6144;

enum class Step : uint8_t { Start, Join, Leave, Off };

struct StepCall {
  Step step;
  const char* ssid;
  const char* password;
  const char* why;
  bool ok;
};

QueueHandle_t stepQueue = nullptr;  // StepCall*
SemaphoreHandle_t stepDone = nullptr;

bool onPsramStack() {
  int probe = 0;
  return esp_ptr_external_ram(&probe);
}

/** Turns Wi-Fi off, saying why in the module log (the console is gone while the module runs). */
void turnOffHere(const char* why) {
  WiFi.disconnect(false);
  WiFi.mode(WIFI_OFF);
  AudioModule::logf("Wi-Fi: off (%s)", why);
}

void runStep(StepCall& call) {
  switch (call.step) {
    case Step::Start:
      call.ok = WiFi.mode(WIFI_STA);
      if (!call.ok) WiFi.mode(WIFI_OFF);  // the driver needs internal RAM for its buffers
      break;
    case Step::Join:
      WiFi.begin(call.ssid, call.password);
      call.ok = true;
      break;
    case Step::Leave:
      WiFi.disconnect(false);
      call.ok = true;
      break;
    case Step::Off:
      turnOffHere(call.why);
      call.ok = true;
      break;
  }
}

void stepTaskMain(void*) {
  for (;;) {
    StepCall* call = nullptr;
    if (xQueueReceive(stepQueue, &call, portMAX_DELAY) != pdTRUE || !call) continue;
    runStep(*call);
    const unsigned spare = static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr));
    if (spare < 768) AudioModule::logf("Wi-Fi: step task stack %u bytes spare", spare);
    xSemaphoreGive(stepDone);
  }
}

void ensureLock() {
  if (!lock) lock = xSemaphoreCreateRecursiveMutex();
  if (!stepQueue) {
    stepDone = xSemaphoreCreateBinary();
    stepQueue = xQueueCreate(1, sizeof(StepCall*));
    if (stepQueue && stepDone) {
      xTaskCreatePinnedToCore(stepTaskMain, "abs_wifi", STEP_TASK_STACK, nullptr, 2, nullptr, 1);
    }
  }
}

/** Runs a step here on an internal stack, or on the abs_wifi task. Callers hold the lock: one at a time. */
bool doStep(const Step step, const char* ssid = nullptr, const char* password = nullptr, const char* why = nullptr) {
  StepCall call{step, ssid, password, why, false};
  if (!onPsramStack()) {
    runStep(call);
    return call.ok;
  }
  StepCall* pointer = &call;
  if (!stepQueue || xQueueSend(stepQueue, &pointer, portMAX_DELAY) != pdTRUE) return false;
  xSemaphoreTake(stepDone, portMAX_DELAY);
  return call.ok;
}

void takeLock() {
  ensureLock();
  xSemaphoreTakeRecursive(lock, portMAX_DELAY);
}

void giveLock() { xSemaphoreGiveRecursive(lock); }

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

}  // namespace

void prepare() {
  ensureLock();
  {
    SdIoMutex::Lock sdLock;
    WIFI_STORE.loadFromFile();
  }
  takeLock();
  networks.clear();
  for (const auto& credential : WIFI_STORE.getCredentials()) {
    if (!credential.ssid.empty()) networks.emplace_back(credential.ssid, credential.password);
  }
  giveLock();
}

bool hasSavedNetworks() {
  takeLock();
  const bool any = !networks.empty();
  giveLock();
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
  takeLock();
  if (WiFi.status() == WL_CONNECTED) {
    ++users;
    giveLock();
    return true;
  }
  if (hotspotActive()) {
    giveLock();
    error = "Wi-Fi is in hotspot mode";
    return false;
  }
  if (networks.empty()) {
    giveLock();
    error = "No saved Wi-Fi network";
    return false;
  }
  if (!doStep(Step::Start)) {
    giveLock();
    error = "Wi-Fi could not start (low memory?)";
    AudioModule::logf("Wi-Fi: could not start (internal RAM %u bytes free, largest block %u)",
                      static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                      static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)));
    return false;
  }
  std::string tried;  // "HighTail not found, Home refused"
  bool anyNotFound = false;
  for (const auto& network : networks) {
    AudioModule::logf("Wi-Fi: joining %s", network.first.c_str());
    doStep(Step::Join, network.first.c_str(), network.second.c_str());
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
      giveLock();
      return true;
    }
    const char* why = joinFailure(status);
    anyNotFound = anyNotFound || status == WL_NO_SSID_AVAIL;
    AudioModule::logf("Wi-Fi: %s %s after %lu ms (status %d)", network.first.c_str(), why,
                      static_cast<unsigned long>(millis() - started), static_cast<int>(status));
    tried += (tried.empty() ? "" : ", ") + network.first + " " + why;
    doStep(Step::Leave);
    vTaskDelay(pdMS_TO_TICKS(100));
  }
  doStep(Step::Off, nullptr, nullptr, "could not join");
  giveLock();
  error = "Could not join Wi-Fi: " + tried + (anyNotFound ? " (hotspot asleep, or 5 GHz only?)" : "");
  return false;
}

void adopt() {
  takeLock();
  if (WiFi.status() == WL_CONNECTED) turnedOn = true;
  giveLock();
}

void offIfUnused() {
  takeLock();
  if (users == 0 && !hotspotActive()) {
    if (WiFi.getMode() != WIFI_OFF) doStep(Step::Off, nullptr, nullptr, "screen closed");
    turnedOn = false;
  }
  giveLock();
}

void release() {
  takeLock();
  if (users > 0) --users;
  if (users == 0 && turnedOn) {
    // Not under a hotspot started since: that is its screen's to turn off.
    if (!hotspotActive()) doStep(Step::Off, nullptr, nullptr, "not needed");
    turnedOn = false;
  }
  giveLock();
}

}  // namespace AbsWifi

#endif  // INX_HAS_AUDIO_MODULE
