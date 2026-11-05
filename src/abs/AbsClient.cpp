/**
 * @file AbsClient.cpp
 * @brief Definitions for the Audiobookshelf API client.
 */

#include "AbsClient.h"

#if INX_HAS_AUDIO_MODULE

#include <Arduino.h>
#include <ArduinoJson.h>
#include <SDCardManager.h>
#include <WiFi.h>
#include <esp_crt_bundle.h>
#include <esp_heap_caps.h>
#include <esp_http_client.h>
#include <esp_tls_errors.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <mbedtls/error.h>
#include <mbedtls/platform.h>

#include <cstdio>
#include <utility>

#include "util/SdIoMutex.h"

namespace Abs {

namespace {

constexpr const char* SETTINGS_FILE = "/.system/audiobookshelf.json";
constexpr size_t MAX_RESPONSE_BYTES = 2 * 1024 * 1024;
constexpr int HTTP_TIMEOUT_MS = 20000;

SemaphoreHandle_t lock = nullptr;         // the fields below
SemaphoreHandle_t refreshLock = nullptr;  // one refresh at a time: refresh tokens rotate
std::string server;
std::string user;
std::string accessToken;
std::string refreshToken;
bool dirty = false;

class Guard {
 public:
  explicit Guard(SemaphoreHandle_t mutex) : mutex_(mutex) { xSemaphoreTake(mutex_, portMAX_DELAY); }
  ~Guard() { xSemaphoreGive(mutex_); }
  Guard(const Guard&) = delete;
  Guard& operator=(const Guard&) = delete;

 private:
  SemaphoreHandle_t mutex_;
};

struct Response {
  int status = 0;
  std::string body;
  bool overflow = false;
};

esp_err_t collectBody(esp_http_client_event_t* event) {
  auto* response = static_cast<Response*>(event->user_data);
  if (event->event_id == HTTP_EVENT_ON_DATA && response) {
    if (response->body.size() + event->data_len > MAX_RESPONSE_BYTES) {
      response->overflow = true;
    } else {
      response->body.append(static_cast<const char*>(event->data), event->data_len);
    }
  }
  return ESP_OK;
}

using Headers = std::vector<std::pair<std::string, std::string>>;

/** "host:port" of a URL, for error messages. */
std::string hostOf(const std::string& url) {
  const size_t start = url.find("://");
  const size_t from = start == std::string::npos ? 0 : start + 3;
  const size_t end = url.find('/', from);
  return url.substr(from, end == std::string::npos ? std::string::npos : end - from);
}

/**
 * Says why a request could not reach the server. esp_http_client reports every such failure as
 * ESP_ERR_HTTP_CONNECT; the cause is in esp-tls's last error: `tlsErr` (ESP_ERR_ESP_TLS_*, what
 * failed), `mbedtlsCode` (mbedTLS's own error, negative) and `certFlags` (certificate checks).
 * The commonest mistake: an address typed without a scheme gets https://, but Audiobookshelf
 * itself serves plain HTTP (port 13378 by default).
 */
std::string connectError(const esp_err_t err, const esp_err_t tlsErr, const int mbedtlsCode, const int certFlags,
                         const std::string& url) {
  const std::string host = hostOf(url);
  const bool https = url.rfind("https://", 0) == 0;
  switch (tlsErr) {
    case ESP_ERR_ESP_TLS_CANNOT_RESOLVE_HOSTNAME: {
      // Say which DNS server was asked: a name that only your own DNS knows (a local override)
      // needs the network to hand that server out, which is what the T5 uses.
      const std::string name = host.substr(0, host.find(':'));
      const bool mdns = name.size() > 6 && name.compare(name.size() - 6, 6, ".local") == 0;
      return "Server name not found: " + name + " (asked DNS " + WiFi.dnsIP(0).toString().c_str() + ", T5 at " +
             WiFi.localIP().toString().c_str() + ")." +
             (mdns ? " Names ending in .local do not work here; use its IP address." : "");
    }
    case ESP_ERR_ESP_TLS_FAILED_CONNECT_TO_HOST:
    case ESP_ERR_ESP_TLS_CONNECTION_TIMEOUT:
      return "Could not reach " + host + ". Check the address and port, and that the T5 is on the same network.";
    default:
      break;
  }
  if (https && certFlags != 0) {
    return "The server's certificate is not trusted (self-signed?). Use a certificate from a public CA, or "
           "http://" +
           host + " on your own network.";
  }
  // The details, so an unusual failure can be told apart (out of memory, a TLS version or cipher
  // the server refuses, the server closing the connection).
  char detail[160];
  char mbedtlsText[80] = "";
  if (mbedtlsCode != 0) mbedtls_strerror(mbedtlsCode, mbedtlsText, sizeof(mbedtlsText));
  std::snprintf(detail, sizeof(detail), " (%s, esp-tls 0x%x, mbedTLS -0x%04x %s; %u KB internal RAM free)",
                esp_err_to_name(err), static_cast<unsigned>(tlsErr), static_cast<unsigned>(-mbedtlsCode), mbedtlsText,
                static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) / 1024));
  if (https && mbedtlsCode != 0) return "Secure connection to " + host + " failed" + detail;
  return "Could not connect to " + host + detail;
}

bool perform(const esp_http_client_method_t method, const std::string& url, const Headers& headers,
             const std::string& body, Response& response, std::string& error) {
  esp_http_client_config_t config = {};
  config.url = url.c_str();
  config.method = method;
  config.event_handler = collectBody;
  config.user_data = &response;
  config.timeout_ms = HTTP_TIMEOUT_MS;
  config.crt_bundle_attach = esp_crt_bundle_attach;
  config.buffer_size = 4096;
  config.buffer_size_tx = 2048;
  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (!client) {
    error = "HTTP client init failed";
    return false;
  }
  esp_http_client_set_header(client, "User-Agent", "inx-pro/" INX_VERSION);
  esp_http_client_set_header(client, "Accept", "application/json");
  for (const auto& header : headers) esp_http_client_set_header(client, header.first.c_str(), header.second.c_str());
  if (!body.empty()) {
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, body.data(), static_cast<int>(body.size()));
  }
  const esp_err_t err = esp_http_client_perform(client);
  response.status = esp_http_client_get_status_code(client);
  int mbedtlsCode = 0;
  int certFlags = 0;
  const esp_err_t tlsErr =
      err != ESP_OK ? esp_http_client_get_and_clear_last_tls_error(client, &mbedtlsCode, &certFlags) : ESP_OK;
  esp_http_client_cleanup(client);
  if (err != ESP_OK) {
    const bool reached = tlsErr == ESP_OK && mbedtlsCode == 0 && certFlags == 0 && err != ESP_ERR_HTTP_CONNECT;
    error = reached ? std::string("Network error: ") + esp_err_to_name(err)
                    : connectError(err, tlsErr, mbedtlsCode, certFlags, url);
    INX_SERIAL.printf("[ABS] %s: %s\n", url.c_str(), error.c_str());
    return false;
  }
  if (response.overflow) {
    error = "Response too large";
    return false;
  }
  return true;
}

std::string bearer(const std::string& token) { return token.empty() ? "" : "Bearer " + token; }

/** Reads accessToken/refreshToken (or the legacy token) out of a /login or /auth/refresh reply. */
bool takeTokens(const std::string& body, std::string& error) {
  JsonDocument filter;
  filter["user"]["accessToken"] = true;
  filter["user"]["refreshToken"] = true;
  filter["user"]["token"] = true;
  JsonDocument doc;
  if (deserializeJson(doc, body, DeserializationOption::Filter(filter)) != DeserializationError::Ok) {
    error = "Unexpected reply from the server";
    return false;
  }
  const char* access = doc["user"]["accessToken"] | "";
  const char* refresh = doc["user"]["refreshToken"] | "";
  const char* legacy = doc["user"]["token"] | "";
  Guard guard(lock);
  accessToken = access[0] ? access : legacy;
  // A refresh reply without a new refresh token keeps the old one (servers older than 2.26).
  if (refresh[0]) refreshToken = refresh;
  if (!refresh[0] && !access[0]) refreshToken.clear();
  dirty = true;
  if (accessToken.empty()) {
    error = "The server did not return a token";
    return false;
  }
  return true;
}

/** Authenticated request with one refresh-and-retry on 401. */
bool api(const esp_http_client_method_t method, const std::string& path, const std::string& body, Response& response,
         std::string& error) {
  for (int attempt = 0; attempt < 2; ++attempt) {
    response = Response{};
    std::string auth = authorization();
    if (auth.empty()) {
      error = "Not signed in";
      return false;
    }
    if (!perform(method, url(path), {{"Authorization", auth}}, body, response, error)) return false;
    if (response.status == 401 && attempt == 0) {
      if (!refreshAccess()) {
        error = "Session expired: sign in again";
        return false;
      }
      continue;
    }
    break;
  }
  if (response.status < 200 || response.status >= 300) {
    error = "Server returned HTTP " + std::to_string(response.status);
    return false;
  }
  return true;
}

std::string normalizeServer(std::string value) {
  while (!value.empty() && (value.back() == '/' || value.back() == ' ')) value.pop_back();
  while (!value.empty() && value.front() == ' ') value.erase(value.begin());
  if (!value.empty() && value.find("://") == std::string::npos) value = "https://" + value;
  return value;
}

void loadSettings() {
  SdIoMutex::Lock sdLock;
  FsFile file = SdMan.open(SETTINGS_FILE, O_RDONLY);
  if (!file) return;
  std::string text;
  char chunk[256];
  int got;
  while ((got = file.read(chunk, sizeof(chunk))) > 0) text.append(chunk, got);
  file.close();
  JsonDocument doc;
  if (deserializeJson(doc, text) != DeserializationError::Ok) return;
  Guard guard(lock);
  server = doc["server"] | "";
  user = doc["username"] | "";
  refreshToken = doc["refreshToken"] | "";
  accessToken = doc["accessToken"] | "";
}

void saveSettings() {
  JsonDocument doc;
  {
    Guard guard(lock);
    doc["server"] = server;
    doc["username"] = user;
    doc["refreshToken"] = refreshToken;
    doc["accessToken"] = accessToken;
    dirty = false;
  }
  std::string text;
  serializeJson(doc, text);
  SdIoMutex::Lock sdLock;
  SdMan.mkdir("/.system");
  FsFile file = SdMan.open(SETTINGS_FILE, O_WRONLY | O_CREAT | O_TRUNC);
  if (!file) return;
  file.write(text.data(), text.size());
  file.close();
}

}  // namespace

/**
 * mbedTLS's allocations, PSRAM first. The prebuilt framework allocates them in internal RAM
 * (CONFIG_MBEDTLS_INTERNAL_MEM_ALLOC), and an HTTPS handshake needs a 16 KB record buffer plus
 * its working memory, ~40-50 KB; with the reader running, the largest internal block was 30 KB,
 * so every HTTPS request failed with "SSL - Memory allocation failed". This is what the
 * framework's own CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC does, with internal RAM as the fallback.
 */
static void* tlsCalloc(const size_t count, const size_t size) {
  void* p = heap_caps_calloc(count, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  return p ? p : heap_caps_calloc(count, size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

static void tlsFree(void* p) { heap_caps_free(p); }

void begin() {
  // Before any TLS session exists: they must be freed by the allocator that made them.
  static bool tlsInPsram = false;
  if (!tlsInPsram) tlsInPsram = mbedtls_platform_set_calloc_free(tlsCalloc, tlsFree) == 0;
  if (!lock) lock = xSemaphoreCreateMutex();
  if (!refreshLock) refreshLock = xSemaphoreCreateMutex();
  loadSettings();
}

void service() {
  bool save = false;
  {
    Guard guard(lock);
    save = dirty;
  }
  if (save) saveSettings();
}

bool configured() {
  Guard guard(lock);
  return !server.empty() && !user.empty();
}

bool signedIn() {
  Guard guard(lock);
  return !refreshToken.empty() || !accessToken.empty();
}

std::string serverUrl() {
  Guard guard(lock);
  return server;
}

std::string username() {
  Guard guard(lock);
  return user;
}

bool signIn(const std::string& serverInput, const std::string& userInput, const std::string& password,
            std::string& error) {
  const std::string base = normalizeServer(serverInput);
  if (base.empty() || userInput.empty()) {
    error = "Server and username are required";
    return false;
  }
  JsonDocument request;
  request["username"] = userInput;
  request["password"] = password;
  std::string body;
  serializeJson(request, body);
  Response response;
  if (!perform(HTTP_METHOD_POST, base + "/login", {{"x-return-tokens", "true"}}, body, response, error)) return false;
  if (response.status == 401) {
    error = "Wrong username or password";
    return false;
  }
  if (response.status < 200 || response.status >= 300) {
    error = "Server returned HTTP " + std::to_string(response.status);
    return false;
  }
  {
    Guard guard(lock);
    server = base;
    user = userInput;
  }
  return takeTokens(response.body, error);
}

void signOut() {
  std::string refresh;
  {
    Guard guard(lock);
    refresh = refreshToken;
  }
  if (!refresh.empty()) {
    Response response;
    std::string ignored;
    perform(HTTP_METHOD_POST, url("/logout"), {{"x-refresh-token", refresh}}, "", response, ignored);
  }
  Guard guard(lock);
  accessToken.clear();
  refreshToken.clear();
  dirty = true;
}

std::string url(const std::string& path) {
  Guard guard(lock);
  return server + path;
}

std::string authorization() {
  Guard guard(lock);
  return bearer(accessToken);
}

bool refreshAccess() {
  // Refresh tokens rotate, so two tasks refreshing at once would invalidate each other. The
  // second one in sees the token already changed and only retries.
  const std::string before = authorization();
  Guard serial(refreshLock);
  if (authorization() != before) return true;
  std::string refresh;
  {
    Guard guard(lock);
    refresh = refreshToken;
  }
  if (refresh.empty()) return false;
  Response response;
  std::string error;
  if (!perform(HTTP_METHOD_POST, url("/auth/refresh"), {{"x-refresh-token", refresh}}, "", response, error)) {
    return false;
  }
  if (response.status == 401) {
    Guard guard(lock);
    accessToken.clear();
    refreshToken.clear();
    dirty = true;
    return false;
  }
  return response.status >= 200 && response.status < 300 && takeTokens(response.body, error);
}

bool libraries(std::vector<Library>& out, std::string& error) {
  Response response;
  if (!api(HTTP_METHOD_GET, "/api/libraries", "", response, error)) return false;
  JsonDocument filter;
  filter["libraries"][0]["id"] = true;
  filter["libraries"][0]["name"] = true;
  filter["libraries"][0]["mediaType"] = true;
  JsonDocument doc;
  if (deserializeJson(doc, response.body, DeserializationOption::Filter(filter)) != DeserializationError::Ok) {
    error = "Unexpected reply from the server";
    return false;
  }
  out.clear();
  for (JsonObject library : doc["libraries"].as<JsonArray>()) {
    if (std::string(library["mediaType"] | "book") != "book") continue;  // podcasts are not books
    out.push_back({library["id"] | "", library["name"] | ""});
  }
  return true;
}

bool items(const std::string& libraryId, const int page, const int perPage, std::vector<ItemSummary>& out, int& total,
           std::string& error) {
  Response response;
  const std::string path = "/api/libraries/" + libraryId + "/items?limit=" + std::to_string(perPage) +
                           "&page=" + std::to_string(page) + "&sort=media.metadata.title&minified=1";
  if (!api(HTTP_METHOD_GET, path, "", response, error)) return false;
  JsonDocument filter;
  filter["total"] = true;
  JsonObject result = filter["results"][0].to<JsonObject>();
  result["id"] = true;
  result["media"]["metadata"]["title"] = true;
  result["media"]["metadata"]["authorName"] = true;
  result["media"]["duration"] = true;
  result["media"]["size"] = true;
  JsonDocument doc;
  if (deserializeJson(doc, response.body, DeserializationOption::Filter(filter)) != DeserializationError::Ok) {
    error = "Unexpected reply from the server";
    return false;
  }
  total = doc["total"] | 0;
  out.clear();
  for (JsonObject entry : doc["results"].as<JsonArray>()) {
    ItemSummary item;
    item.id = entry["id"] | "";
    item.title = entry["media"]["metadata"]["title"] | "";
    item.author = entry["media"]["metadata"]["authorName"] | "";
    item.durationSec = static_cast<uint32_t>(entry["media"]["duration"] | 0.0);
    item.sizeBytes = entry["media"]["size"] | 0ull;
    out.push_back(item);
  }

  // The user's progress for the markers in the list. One request for the whole page.
  Response me;
  std::string ignored;
  if (api(HTTP_METHOD_GET, "/api/me", "", me, ignored)) {
    JsonDocument meFilter;
    JsonObject progressFilter = meFilter["mediaProgress"][0].to<JsonObject>();
    progressFilter["libraryItemId"] = true;
    progressFilter["progress"] = true;
    progressFilter["isFinished"] = true;
    JsonDocument meDoc;
    if (deserializeJson(meDoc, me.body, DeserializationOption::Filter(meFilter)) == DeserializationError::Ok) {
      for (JsonObject p : meDoc["mediaProgress"].as<JsonArray>()) {
        const std::string id = p["libraryItemId"] | "";
        for (ItemSummary& item : out) {
          if (item.id != id) continue;
          item.progress = p["progress"] | 0.0f;
          item.finished = p["isFinished"] | false;
        }
      }
    }
  }
  return true;
}

bool item(const std::string& itemId, ItemDetail& out, std::string& error) {
  Response response;
  if (!api(HTTP_METHOD_GET, "/api/items/" + itemId + "?expanded=1", "", response, error)) return false;
  JsonDocument filter;
  filter["id"] = true;
  filter["media"]["metadata"]["title"] = true;
  filter["media"]["metadata"]["authorName"] = true;
  filter["media"]["duration"] = true;
  JsonObject track = filter["media"]["tracks"][0].to<JsonObject>();
  track["index"] = true;
  track["startOffset"] = true;
  track["duration"] = true;
  track["contentUrl"] = true;
  track["metadata"]["filename"] = true;
  track["metadata"]["size"] = true;
  JsonObject chapter = filter["media"]["chapters"][0].to<JsonObject>();
  chapter["start"] = true;
  chapter["title"] = true;
  JsonDocument doc;
  if (deserializeJson(doc, response.body, DeserializationOption::Filter(filter)) != DeserializationError::Ok) {
    error = "Unexpected reply from the server";
    return false;
  }
  out = ItemDetail{};
  out.id = doc["id"] | itemId.c_str();
  out.title = doc["media"]["metadata"]["title"] | "";
  out.author = doc["media"]["metadata"]["authorName"] | "";
  out.durationMs = static_cast<uint32_t>((doc["media"]["duration"] | 0.0) * 1000.0);
  for (JsonObject t : doc["media"]["tracks"].as<JsonArray>()) {
    Track entry;
    entry.index = t["index"] | 0;
    entry.contentUrl = t["contentUrl"] | "";
    entry.filename = t["metadata"]["filename"] | "";
    entry.sizeBytes = t["metadata"]["size"] | 0ull;
    entry.startOffsetMs = static_cast<uint32_t>((t["startOffset"] | 0.0) * 1000.0);
    entry.durationMs = static_cast<uint32_t>((t["duration"] | 0.0) * 1000.0);
    out.tracks.push_back(entry);
  }
  for (JsonObject c : doc["media"]["chapters"].as<JsonArray>()) {
    out.chapters.push_back({static_cast<uint32_t>((c["start"] | 0.0) * 1000.0), c["title"] | ""});
  }
  if (out.tracks.empty()) {
    error = "This item has no audio tracks";
    return false;
  }
  return true;
}

bool progress(const std::string& itemId, Progress& out, std::string& error) {
  out = Progress{};
  Response response;
  if (!api(HTTP_METHOD_GET, "/api/me/progress/" + itemId, "", response, error)) {
    // 404: never played on any device. Not an error.
    if (response.status == 404) {
      error.clear();
      return true;
    }
    return false;
  }
  JsonDocument doc;
  if (deserializeJson(doc, response.body) != DeserializationError::Ok) {
    error = "Unexpected reply from the server";
    return false;
  }
  out.exists = true;
  out.currentMs = static_cast<uint32_t>((doc["currentTime"] | 0.0) * 1000.0);
  out.durationMs = static_cast<uint32_t>((doc["duration"] | 0.0) * 1000.0);
  out.finished = doc["isFinished"] | false;
  out.lastUpdateMs = doc["lastUpdate"] | 0ull;
  return true;
}

bool updateProgress(const std::string& itemId, const uint32_t currentMs, const uint32_t durationMs, const bool finished,
                    std::string& error) {
  JsonDocument request;
  request["currentTime"] = currentMs / 1000.0;
  request["duration"] = durationMs / 1000.0;
  if (durationMs) request["progress"] = static_cast<double>(currentMs) / durationMs;
  request["isFinished"] = finished;
  std::string body;
  serializeJson(request, body);
  Response response;
  return api(HTTP_METHOD_PATCH, "/api/me/progress/" + itemId, body, response, error);
}

}  // namespace Abs

#endif  // INX_HAS_AUDIO_MODULE
