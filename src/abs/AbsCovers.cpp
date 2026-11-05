/**
 * @file AbsCovers.cpp
 * @brief Definitions for the Audiobookshelf cover cache.
 */

#include "AbsCovers.h"

#if INX_HAS_AUDIO_MODULE

#include <Arduino.h>
#include <SDCardManager.h>
#include <esp_crt_bundle.h>
#include <esp_heap_caps.h>
#include <esp_http_client.h>
#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include <atomic>
#include <cstring>
#include <set>

#include "AbsClient.h"
#include "AbsWifi.h"
#include "util/SdIoMutex.h"

namespace AbsCovers {

namespace {

constexpr const char* DIRECTORY = "/.system/cache/abs-covers";
constexpr int COVER_WIDTH = 240;
constexpr size_t MAX_COVER_BYTES = 256 * 1024;
constexpr int QUEUE_LENGTH = 32;
constexpr int HTTP_TIMEOUT_MS = 15000;
constexpr size_t ID_CHARS = 64;  // Audiobookshelf ids are UUIDs (36)

struct Request {
  char id[ID_CHARS];
};

struct Result {
  char id[ID_CHARS];
  uint8_t* data;  // PSRAM; nullptr when there is no cover
  size_t length;
  bool failed;  // the server had none or the fetch failed (not: skipped, which is asked again later)
};

QueueHandle_t requests = nullptr;
QueueHandle_t results = nullptr;
TaskHandle_t task = nullptr;
std::atomic<uint32_t> generation{0};  // bumped by cancel(): requests from before are skipped
uint32_t written = 0;                 // main loop: covers written since boot

// Main loop only: ids requested and not yet written (or failed), so each is asked for once.
std::set<std::string> pending;
std::set<std::string> failed;  // not asked for again until the next boot

struct Body {
  uint8_t* data = nullptr;
  size_t length = 0;
  bool overflow = false;
};

esp_err_t collect(esp_http_client_event_t* event) {
  if (event->event_id != HTTP_EVENT_ON_DATA || !event->user_data) return ESP_OK;
  auto* body = static_cast<Body*>(event->user_data);
  if (!body->data || body->length + event->data_len > MAX_COVER_BYTES) {
    body->overflow = true;
    return ESP_OK;
  }
  std::memcpy(body->data + body->length, event->data, event->data_len);
  body->length += event->data_len;
  return ESP_OK;
}

/** Fetches one cover on `client` (kept alive between covers). The buffer goes to the result. */
void fetch(esp_http_client_handle_t client, const char* id) {
  Result result{};
  std::strncpy(result.id, id, sizeof(result.id) - 1);
  Body body;
  body.data = static_cast<uint8_t*>(heap_caps_malloc(MAX_COVER_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (body.data) {
    const std::string url =
        Abs::url("/api/items/" + std::string(id) + "/cover?width=" + std::to_string(COVER_WIDTH) + "&format=jpeg");
    esp_http_client_set_url(client, url.c_str());
    esp_http_client_set_user_data(client, &body);
    esp_err_t err = ESP_OK;
    int status = 0;
    for (int attempt = 0; attempt < 2; ++attempt) {
      const std::string authorization = Abs::authorization();
      esp_http_client_set_header(client, "Authorization", authorization.c_str());
      body.length = 0;
      body.overflow = false;
      err = esp_http_client_perform(client);
      status = esp_http_client_get_status_code(client);
      // A 401 comes back as ESP_ERR_NOT_SUPPORTED from ESP-IDF 5 (see AbsClient's perform()).
      if (err == ESP_ERR_NOT_SUPPORTED && status == 401) err = ESP_OK;
      if (!(err == ESP_OK && status == 401 && attempt == 0 && Abs::refreshAccess())) break;
    }
    if (err == ESP_OK && status == 200 && !body.overflow && body.length > 0) {
      result.data = body.data;
      result.length = body.length;
    } else {
      INX_SERIAL.printf("[ABSCOVER] %s: %s, HTTP %d\n", id, esp_err_to_name(err), status);
      heap_caps_free(body.data);
      result.failed = true;
    }
  } else {
    result.failed = true;
  }
  if (xQueueSend(results, &result, pdMS_TO_TICKS(1000)) != pdTRUE) heap_caps_free(result.data);
}

void coverTask(void*) {
  for (;;) {
    Request request;
    xQueueReceive(requests, &request, portMAX_DELAY);
    // One connection for this run of requests; closed when the queue goes quiet.
    const uint32_t started = generation;
    esp_http_client_config_t config = {};
    const std::string first = Abs::url("/");
    config.url = first.c_str();
    config.timeout_ms = HTTP_TIMEOUT_MS;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.event_handler = collect;
    config.buffer_size = 4096;
    config.buffer_size_tx = 2048;
    config.keep_alive_enable = true;
    esp_http_client_handle_t client = AbsWifi::connected() ? esp_http_client_init(&config) : nullptr;
    if (client) esp_http_client_set_header(client, "User-Agent", "inx-pro/" INX_VERSION);
    for (bool more = true; more;) {
      if (client && generation == started) {
        fetch(client, request.id);
      } else {
        Result skipped{};
        std::strncpy(skipped.id, request.id, sizeof(skipped.id) - 1);
        xQueueSend(results, &skipped, pdMS_TO_TICKS(1000));  // so the main loop forgets it
      }
      more = xQueueReceive(requests, &request, pdMS_TO_TICKS(500)) == pdTRUE;
    }
    if (client) esp_http_client_cleanup(client);
  }
}

bool ensureTask() {
  if (task) return true;
  requests = xQueueCreate(QUEUE_LENGTH, sizeof(Request));
  results = xQueueCreate(QUEUE_LENGTH, sizeof(Result));
  if (!requests || !results) return false;
  if (xTaskCreatePinnedToCoreWithCaps(coverTask, "abs_covers", 8192, nullptr, 1, &task, tskNO_AFFINITY,
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
    task = nullptr;
    return false;
  }
  return true;
}

}  // namespace

std::string path(const std::string& itemId) { return std::string(DIRECTORY) + "/" + itemId + ".jpg"; }

bool cached(const std::string& itemId) {
  SdIoMutex::Lock lock;
  return SdMan.exists(path(itemId).c_str());
}

void request(const std::vector<std::string>& itemIds) {
  if (!AbsWifi::connected() || !ensureTask()) return;
  for (const std::string& id : itemIds) {
    if (id.empty() || id.size() >= ID_CHARS || pending.count(id) || failed.count(id) || cached(id)) continue;
    Request r{};
    std::strncpy(r.id, id.c_str(), sizeof(r.id) - 1);
    if (xQueueSend(requests, &r, 0) != pdTRUE) break;  // full: the rest next time
    pending.insert(id);
  }
}

void cancel() {
  generation = generation + 1;
  if (!requests) return;
  // Requests still queued come back as skipped results, which clears them from `pending`.
}

uint32_t changeCounter() { return written; }

void service() {
  if (!results) return;
  Result result;
  while (xQueueReceive(results, &result, 0) == pdTRUE) {
    pending.erase(result.id);
    if (!result.data) {
      // Skipped (cancelled, no Wi-Fi) is asked for again later; a failed fetch (no cover on the
      // server) is not, until the next boot.
      if (result.failed) failed.insert(result.id);
      continue;
    }
    {
      SdIoMutex::Lock lock;
      SdMan.mkdir("/.system/cache");
      SdMan.mkdir(DIRECTORY);
      // Through a temporary file, so a half-written cover never looks cached.
      const std::string final = path(result.id);
      const std::string temporary = final + ".tmp";
      FsFile file = SdMan.open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC);
      bool ok = file && file.write(result.data, result.length) == result.length;
      if (file) file.close();
      if (ok) {
        SdMan.remove(final.c_str());
        ok = SdMan.rename(temporary.c_str(), final.c_str());
      }
      if (ok) {
        ++written;
      } else {
        SdMan.remove(temporary.c_str());
        failed.insert(result.id);
      }
    }
    heap_caps_free(result.data);
  }
}

}  // namespace AbsCovers

#endif  // INX_HAS_AUDIO_MODULE
