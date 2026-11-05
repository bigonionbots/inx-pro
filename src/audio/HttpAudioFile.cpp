/**
 * @file HttpAudioFile.cpp
 * @brief Definitions for HttpAudioFile.
 */

#include "HttpAudioFile.h"

#if INX_HAS_AUDIO_MODULE

#include <Arduino.h>
#include <esp_crt_bundle.h>
#include <esp_heap_caps.h>
#include <esp_http_client.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>

#include "abs/AbsClient.h"

namespace {

constexpr int ATTEMPTS = 4;
constexpr uint32_t FIRST_RETRY_DELAY_MS = 1000;
constexpr int HTTP_TIMEOUT_MS = 15000;

struct HeaderCapture {
  uint64_t totalSize = 0;
};

// "Content-Range: bytes 0-131071/52428800" carries the full size.
esp_err_t onHttpEvent(esp_http_client_event_t* event) {
  if (event->event_id != HTTP_EVENT_ON_HEADER || !event->user_data) return ESP_OK;
  if (strcasecmp(event->header_key, "Content-Range") != 0) return ESP_OK;
  const char* slash = strchr(event->header_value, '/');
  if (slash && slash[1] != '*')
    static_cast<HeaderCapture*>(event->user_data)->totalSize = strtoull(slash + 1, nullptr, 10);
  return ESP_OK;
}

}  // namespace

HttpAudioFile::HttpAudioFile(std::string url) : url_(std::move(url)) {}

HttpAudioFile::~HttpAudioFile() {
  for (Block& b : blocks_) heap_caps_free(b.data);
}

bool HttpAudioFile::fetchOnce(const uint64_t start, const size_t length, Block& into, int& status) {
  HeaderCapture capture;
  esp_http_client_config_t config = {};
  config.url = url_.c_str();
  config.timeout_ms = HTTP_TIMEOUT_MS;
  config.crt_bundle_attach = esp_crt_bundle_attach;
  config.buffer_size = 4096;
  config.buffer_size_tx = 2048;
  config.event_handler = onHttpEvent;
  config.user_data = &capture;
  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (!client) {
    lastError_ = "HTTP client init failed";
    return false;
  }
  char range[48];
  snprintf(range, sizeof(range), "bytes=%llu-%llu", static_cast<unsigned long long>(start),
           static_cast<unsigned long long>(start + length - 1));
  esp_http_client_set_header(client, "Range", range);
  esp_http_client_set_header(client, "User-Agent", "inx-pro/" INX_VERSION);
  const std::string auth = Abs::authorization();
  if (!auth.empty()) esp_http_client_set_header(client, "Authorization", auth.c_str());

  bool ok = false;
  status = 0;
  const auto describe = [&](const esp_err_t err) {
    int mbedtlsCode = 0;
    int certFlags = 0;
    const esp_err_t tlsErr = esp_http_client_get_and_clear_last_tls_error(client, &mbedtlsCode, &certFlags);
    lastError_ = Abs::describeConnectFailure(err, tlsErr, mbedtlsCode, certFlags, url_);
    INX_SERIAL.printf("[HTTPAUDIO] %s\n", lastError_.c_str());
  };
  const esp_err_t opened = esp_http_client_open(client, 0);
  if (opened == ESP_OK) {
    const int64_t contentLength = esp_http_client_fetch_headers(client);
    status = esp_http_client_get_status_code(client);
    if (contentLength < 0 && status <= 0) {
      // Connected, but no response came back (the "HTTP -1" this used to report).
      describe(ESP_FAIL);
      lastError_ = "No response from the server: " + lastError_;
      esp_http_client_close(client);
      esp_http_client_cleanup(client);
      return false;
    }
    // 206 is the normal answer. A server that ignores Range sends 200 and the whole file, which is
    // only usable for the first block.
    if (status == 206 || (status == 200 && start == 0)) {
      size_t got = 0;
      while (got < length && !cancelled_) {
        const int n = esp_http_client_read(client, reinterpret_cast<char*>(into.data) + got, length - got);
        if (n <= 0) break;
        got += static_cast<size_t>(n);
      }
      into.length = got;
      if (capture.totalSize) {
        size_ = capture.totalSize;
      } else if (status == 200 && contentLength > 0) {
        size_ = static_cast<uint64_t>(contentLength);
      }
      // A short block is only fine at the end of the file.
      ok = got == length || (size_ && start + got >= size_);
      if (!ok) lastError_ = "connection dropped";
    } else if (status == 416) {
      into.length = 0;  // past the end
      ok = true;
    } else {
      lastError_ = "HTTP " + std::to_string(status);
    }
  } else {
    describe(opened);
  }
  esp_http_client_close(client);
  esp_http_client_cleanup(client);
  return ok;
}

bool HttpAudioFile::fetch(const uint64_t index, Block& into) {
  if (!into.data) {
    into.data = static_cast<uint8_t*>(heap_caps_malloc(BLOCK_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!into.data) {
      lastError_ = "out of memory";
      return false;
    }
  }
  const uint64_t start = index * BLOCK_BYTES;
  size_t length = BLOCK_BYTES;
  if (size_ && start + length > size_) length = static_cast<size_t>(size_ - start);
  uint32_t delayMs = FIRST_RETRY_DELAY_MS;
  for (int attempt = 0; attempt < ATTEMPTS && !cancelled_; ++attempt) {
    int status = 0;
    if (fetchOnce(start, length, into, status)) {
      into.index = index;
      return true;
    }
    if (status == 401 && Abs::refreshAccess()) continue;  // new access token: retry at once
    if (status == 401 || status == 403 || status == 404) break;
    vTaskDelay(pdMS_TO_TICKS(delayMs));
    delayMs *= 2;
  }
  into.index = UINT64_MAX;
  return false;
}

HttpAudioFile::Block* HttpAudioFile::block(const uint64_t index) {
  Block* victim = &blocks_[0];
  for (Block& b : blocks_) {
    if (b.index == index) {
      b.lastUse = ++useCounter_;
      return &b;
    }
    if (b.lastUse < victim->lastUse) victim = &b;
  }
  if (!fetch(index, *victim)) return nullptr;
  victim->lastUse = ++useCounter_;
  return victim;
}

bool HttpAudioFile::open(std::string& error) {
  if (!block(0)) {
    error = lastError_.empty() ? "cannot fetch" : lastError_;
    return false;
  }
  if (size_ == 0) {
    error = "server did not report the file size";
    return false;
  }
  return true;
}

bool HttpAudioFile::seekSet(const uint64_t position) {
  position_ = position;
  return position <= size_;
}

int HttpAudioFile::read(void* buffer, const size_t length) {
  size_t done = 0;
  auto* out = static_cast<uint8_t*>(buffer);
  while (done < length && position_ < size_) {
    if (cancelled_) return done > 0 ? static_cast<int>(done) : -1;
    const uint64_t index = position_ / BLOCK_BYTES;
    const Block* b = block(index);
    if (!b) {
      failed_ = true;
      return done > 0 ? static_cast<int>(done) : -1;
    }
    const size_t offset = static_cast<size_t>(position_ - index * BLOCK_BYTES);
    if (offset >= b->length) break;  // short last block
    const size_t n = std::min(length - done, b->length - offset);
    std::memcpy(out + done, b->data + offset, n);
    done += n;
    position_ += n;
  }
  return static_cast<int>(done);
}

#endif  // INX_HAS_AUDIO_MODULE
