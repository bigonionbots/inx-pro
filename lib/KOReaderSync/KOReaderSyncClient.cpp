/**
 * @file KOReaderSyncClient.cpp
 * @brief Definitions for KOReaderSyncClient using native esp_http_client.
 */

#include "KOReaderSyncClient.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HardwareSerial.h>
#include <Utf8.h>

#include <cstdint>
#include <ctime>

#include "KOReaderCredentialStore.h"
#ifndef SIMULATOR
#include "esp_heap_caps.h"
#include "esp_http_client.h"

extern "C" {
extern esp_err_t esp_crt_bundle_attach(void* conf);
}
#endif

int KOReaderSyncClient::lastHttpCode = 0;
size_t KOReaderSyncClient::lowMemoryBlock = 0;
size_t KOReaderSyncClient::lowMemoryFree = 0;

std::string KOReaderSyncClient::describe(const Error error) {
  std::string text = errorString(error);
  if (error == LOW_MEMORY) {
    char figures[64];
    snprintf(figures, sizeof(figures), " (largest block %u KB, %u KB free)",
             static_cast<unsigned>(lowMemoryBlock / 1024), static_cast<unsigned>(lowMemoryFree / 1024));
    text += figures;
  }
  return text;
}

namespace {

constexpr char DEVICE_NAME[] = "CrossPoint";
constexpr char DEVICE_ID[] = "crosspoint-reader";
// Over 4 KB, so malloc() puts them in PSRAM (CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL is 4096): at 2 KB
// both landed in internal RAM, which TLS itself needs.
constexpr int HTTP_RX_BUF_SIZE = 8192;
constexpr int HTTP_TX_BUF_SIZE = 4352;
/**
 * The old check wanted 55 KB of internal heap in total, which a device running the USB host and an
 * audio stream (17-40 KB free, the Audiobookshelf client doing TLS fine) never has. Allocations over
 * 4 KB go to PSRAM, so all TLS takes from internal RAM is pieces of up to 4 KB: below a 4 KB largest
 * free block it cannot work; above it, try (and report a real out-of-memory failure).
 */
constexpr size_t MIN_INTERNAL_BLOCK_FOR_TLS = 4 * 1024;
constexpr size_t METADATA_FIELD_MAX_BYTES = 512;
/** doRequest(): failed for lack of memory (other failures are -1). */
constexpr int LOW_MEMORY_STATUS = -2;

size_t freeInternal() {
#ifdef SIMULATOR
  return SIZE_MAX;
#else
  return heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#endif
}

size_t largestInternalBlock() {
#ifdef SIMULATOR
  return SIZE_MAX;
#else
  return heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#endif
}

/** False (and logged) when internal RAM is too fragmented for a TLS handshake. */
bool enoughMemoryForTls() {
  const size_t block = largestInternalBlock();
  if (block >= MIN_INTERNAL_BLOCK_FOR_TLS) return true;
  KOReaderSyncClient::lowMemoryBlock = block;
  KOReaderSyncClient::lowMemoryFree = freeInternal();
  INX_SERIAL.printf("[%lu] [KOSync] Insufficient memory for TLS: largest internal block %u bytes\n", millis(),
                    static_cast<unsigned>(block));
  return false;
}

void addMetadataField(JsonObject& metadata, const char* key, std::string value) {
  if (value.empty()) {
    return;
  }
  utf8TruncateBytes(value, METADATA_FIELD_MAX_BYTES);
  metadata[key] = value;
}

#ifndef SIMULATOR

struct KoreaderCtx {
  std::string* responseBody;
};

esp_err_t koreaderEventHandler(esp_http_client_event_t* event) {
  if (event->event_id == HTTP_EVENT_ON_DATA && event->data && event->data_len > 0) {
    auto* ctx = static_cast<KoreaderCtx*>(event->user_data);
    if (ctx && ctx->responseBody) {
      ctx->responseBody->append(static_cast<const char*>(event->data), event->data_len);
    }
  }
  return ESP_OK;
}

int doRequest(const std::string& url, const std::string& method, const std::string* body, std::string* responseBody) {
  KoreaderCtx ctx = {responseBody};
  esp_http_client_config_t cfg = {};
  cfg.url = url.c_str();
  cfg.event_handler = koreaderEventHandler;
  cfg.user_data = &ctx;
  cfg.timeout_ms = 15000;
  cfg.crt_bundle_attach = esp_crt_bundle_attach;
  cfg.keep_alive_enable = false;
  cfg.buffer_size = HTTP_RX_BUF_SIZE;
  cfg.buffer_size_tx = HTTP_TX_BUF_SIZE;
  cfg.disable_auto_redirect = false;
  cfg.username = KOREADER_STORE.getUsername().c_str();
  cfg.password = KOREADER_STORE.getPassword().c_str();
  cfg.auth_type = HTTP_AUTH_TYPE_BASIC;

  esp_http_client_handle_t client = esp_http_client_init(&cfg);
  if (!client) {
    INX_SERIAL.printf("[%lu] [KOSync] Failed to init HTTP client\n", millis());
    return -1;
  }

  esp_http_client_set_header(client, "Accept", "application/vnd.koreader.v1+json");
  esp_http_client_set_header(client, "x-auth-user", KOREADER_STORE.getUsername().c_str());
  esp_http_client_set_header(client, "x-auth-key", KOREADER_STORE.getMd5Password().c_str());

  if (method == "POST") {
    esp_http_client_set_method(client, HTTP_METHOD_POST);
  } else if (method == "PUT") {
    esp_http_client_set_method(client, HTTP_METHOD_PUT);
  } else {
    esp_http_client_set_method(client, HTTP_METHOD_GET);
  }

  if (body) {
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, body->c_str(), body->size());
  }

  esp_err_t err = esp_http_client_perform(client);
  const int status = esp_http_client_get_status_code(client);
  KOReaderSyncClient::lastHttpCode = status;
  if (err != ESP_OK) {
    INX_SERIAL.printf("[%lu] [KOSync] perform failed: %s status=%d, largest internal block %u\n", millis(),
                      esp_err_to_name(err), status, static_cast<unsigned>(largestInternalBlock()));
    // Out of memory during the handshake reads as a connection failure: say so, as a retry may work.
    // The figures are taken before cleanup frees the client's memory.
    const size_t block = largestInternalBlock();
    const size_t free = freeInternal();
    esp_http_client_cleanup(client);
    if (err == ESP_ERR_NO_MEM || block < MIN_INTERNAL_BLOCK_FOR_TLS) {
      KOReaderSyncClient::lowMemoryBlock = block;
      KOReaderSyncClient::lowMemoryFree = free;
      return LOW_MEMORY_STATUS;
    }
    return -1;
  }

  esp_http_client_cleanup(client);
  return status;
}

#else

int doRequest(const std::string& url, const std::string& method, const std::string* body, std::string* responseBody) {
  (void)url;
  (void)method;
  (void)body;
  (void)responseBody;
  INX_SERIAL.printf("[%lu] [KOSync] Simulator does not support KOReaderSync HTTP requests\n", millis());
  return -1;
}

#endif

}  // namespace

KOReaderSyncClient::Error KOReaderSyncClient::authenticate() {
  lastHttpCode = 0;
  if (!KOREADER_STORE.hasCredentials()) {
    INX_SERIAL.printf("[%lu] [KOSync] No credentials configured\n", millis());
    return NO_CREDENTIALS;
  }

  std::string url = KOREADER_STORE.getBaseUrl() + "/users/auth";
  if (!enoughMemoryForTls()) return LOW_MEMORY;

  const int httpCode = doRequest(url, "GET", nullptr, nullptr);
  INX_SERIAL.printf("[%lu] [KOSync] Auth response: %d\n", millis(), httpCode);

  if (httpCode == 200) {
    return OK;
  } else if (httpCode == 401) {
    return AUTH_FAILED;
  } else if (httpCode == 404) {
    return NOT_FOUND;
  } else if (httpCode < 0) {
    return httpCode == LOW_MEMORY_STATUS ? LOW_MEMORY : NETWORK_ERROR;
  }
  return SERVER_ERROR;
}

KOReaderSyncClient::Error KOReaderSyncClient::createUser() {
  lastHttpCode = 0;
  if (!KOREADER_STORE.hasCredentials()) {
    INX_SERIAL.printf("[%lu] [KOSync] No credentials configured\n", millis());
    return NO_CREDENTIALS;
  }

  const std::string url = KOREADER_STORE.getBaseUrl() + "/users/create";
  if (!enoughMemoryForTls()) return LOW_MEMORY;

  JsonDocument doc;
  doc["username"] = KOREADER_STORE.getUsername();
  doc["password"] = KOREADER_STORE.getMd5Password();
  std::string body;
  serializeJson(doc, body);

  INX_SERIAL.printf("[%lu] [KOSync] Creating account at %s\n", millis(), url.c_str());
  const int httpCode = doRequest(url, "POST", &body, nullptr);
  INX_SERIAL.printf("[%lu] [KOSync] Create user response: %d\n", millis(), httpCode);

  if (httpCode >= 200 && httpCode < 300) {
    return OK;
  }
  if (httpCode == 402) {
    return USER_EXISTS;
  }
  if (httpCode < 0) {
    return httpCode == LOW_MEMORY_STATUS ? LOW_MEMORY : NETWORK_ERROR;
  }
  return SERVER_ERROR;
}

KOReaderSyncClient::Error KOReaderSyncClient::getProgress(const std::string& documentHash,
                                                          KOReaderProgress& outProgress) {
  lastHttpCode = 0;
  if (!KOREADER_STORE.hasCredentials()) {
    INX_SERIAL.printf("[%lu] [KOSync] No credentials configured\n", millis());
    return NO_CREDENTIALS;
  }

  std::string url = KOREADER_STORE.getBaseUrl() + "/syncs/progress/" + documentHash;
  if (!enoughMemoryForTls()) return LOW_MEMORY;

  std::string responseBody;
  const int httpCode = doRequest(url, "GET", nullptr, &responseBody);

  if (httpCode == 200) {
    JsonDocument doc;
    const DeserializationError error = deserializeJson(doc, responseBody);

    if (error) {
      INX_SERIAL.printf("[%lu] [KOSync] JSON parse failed: %s\n", millis(), error.c_str());
      return JSON_ERROR;
    }

    outProgress.document = documentHash;
    outProgress.progress = doc["progress"].as<std::string>();
    outProgress.percentage = doc["percentage"].as<float>();
    outProgress.device = doc["device"].as<std::string>();
    outProgress.deviceId = doc["device_id"].as<std::string>();
    outProgress.timestamp = doc["timestamp"].as<int64_t>();

    INX_SERIAL.printf("[%lu] [KOSync] Got progress: %.2f%% at %s\n", millis(), outProgress.percentage * 100,
                      outProgress.progress.c_str());
    return OK;
  }

  INX_SERIAL.printf("[%lu] [KOSync] Get progress response: %d\n", millis(), httpCode);

  if (httpCode == 401) {
    return AUTH_FAILED;
  } else if (httpCode == 404) {
    return NOT_FOUND;
  } else if (httpCode < 0) {
    return httpCode == LOW_MEMORY_STATUS ? LOW_MEMORY : NETWORK_ERROR;
  }
  return SERVER_ERROR;
}

KOReaderSyncClient::Error KOReaderSyncClient::updateProgress(const KOReaderProgress& progress) {
  lastHttpCode = 0;
  if (!KOREADER_STORE.hasCredentials()) {
    INX_SERIAL.printf("[%lu] [KOSync] No credentials configured\n", millis());
    return NO_CREDENTIALS;
  }

  std::string url = KOREADER_STORE.getBaseUrl() + "/syncs/progress";
  if (!enoughMemoryForTls()) return LOW_MEMORY;

  JsonDocument doc;
  doc["document"] = progress.document;
  doc["progress"] = progress.progress;
  doc["percentage"] = progress.percentage;
  doc["device"] = DEVICE_NAME;
  doc["device_id"] = DEVICE_ID;

  if (!progress.title.empty() || !progress.authors.empty() || !progress.filename.empty()) {
    JsonObject metadata = doc["metadata"].to<JsonObject>();
    addMetadataField(metadata, "filename", progress.filename);
    addMetadataField(metadata, "title", progress.title);
    addMetadataField(metadata, "authors", progress.authors);
  }

  std::string body;
  serializeJson(doc, body);

  INX_SERIAL.printf("[%lu] [KOSync] Request body: %s\n", millis(), body.c_str());

  const int httpCode = doRequest(url, "PUT", &body, nullptr);
  INX_SERIAL.printf("[%lu] [KOSync] Update progress response: %d\n", millis(), httpCode);

  if (httpCode == 200 || httpCode == 202) {
    return OK;
  } else if (httpCode == 401) {
    return AUTH_FAILED;
  } else if (httpCode == 404) {
    return NOT_FOUND;
  } else if (httpCode < 0) {
    return httpCode == LOW_MEMORY_STATUS ? LOW_MEMORY : NETWORK_ERROR;
  }
  return SERVER_ERROR;
}

const char* KOReaderSyncClient::errorString(Error error) {
  switch (error) {
    case OK:
      return "Success";
    case NO_CREDENTIALS:
      return "No credentials configured";
    case NETWORK_ERROR:
      return "Network error";
    case AUTH_FAILED:
      return "Authentication failed";
    case SERVER_ERROR:
      return "Server error (try again later)";
    case JSON_ERROR:
      return "JSON parse error";
    case NOT_FOUND:
      return "No progress found (first time reading this book?)";
    case LOW_MEMORY:
      return "Not enough memory for sync - please retry";
    case USER_EXISTS:
      return "Username is already registered";
    default:
      return "Unknown error";
  }
}
