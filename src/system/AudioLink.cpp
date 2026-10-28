/**
 * @file AudioLink.cpp
 * @brief Definitions for the T5 end of the audio module link.
 */

#include "AudioLink.h"

#if INX_HAS_AUDIO_MODULE

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <algorithm>
#include <atomic>
#include <cstring>

#include "AudioModule.h"
#include "UsbSerialHost.h"

namespace AudioLink {

namespace {

using audiolink::Type;

constexpr uint32_t PING_INTERVAL_MS = 500;
constexpr uint32_t WRITE_TIMEOUT_MS = 200;
constexpr size_t READ_CHUNK = 512;
constexpr size_t MAX_BT_DEVICES = 32;
constexpr UBaseType_t TASK_PRIORITY = 4;
constexpr uint32_t TASK_STACK = 4096;

struct Slot {
  uint8_t seq;
  uint16_t size;
  uint16_t audioBytes;
  uint8_t frame[audiolink::MAX_FRAME];
};

SemaphoreHandle_t lock = nullptr;      // everything below
SemaphoreHandle_t progress = nullptr;  // given on every ACK, so senders wake up
Slot* window = nullptr;
size_t windowHead = 0;
size_t windowCount = 0;
uint8_t nextSeq = 0;
uint32_t rxFree = 0;
uint32_t inflightAudio = 0;
uint32_t inflightBytes = 0;
uint32_t lastFastResendMs = 0;
uint32_t lastProgressMs = 0;
uint32_t lastPingMs = 0;
bool linkReady = false;
bool pingOutstanding = false;
uint32_t session = 0;
audiolink::HelloPayload helloPayload{};
audiolink::StatusPayload lastStatus{};
uint32_t lastStatusMs = 0;
bool statusValid = false;
std::vector<BtDevice> devices;
Counters linkCounters;
audiolink::Parser* parser = nullptr;
std::atomic<bool> suspended{false};
TaskHandle_t task = nullptr;

class Guard {
 public:
  Guard() { xSemaphoreTake(lock, portMAX_DELAY); }
  ~Guard() { xSemaphoreGive(lock); }
  Guard(const Guard&) = delete;
  Guard& operator=(const Guard&) = delete;
};

void clearWindowLocked() {
  windowHead = 0;
  windowCount = 0;
  inflightAudio = 0;
  inflightBytes = 0;
  rxFree = 0;
}

void resetSessionLocked() {
  linkReady = false;
  pingOutstanding = false;
  statusValid = false;
  clearWindowLocked();
}

void sendPingLocked() {
  uint8_t frame[audiolink::HEADER_BYTES + audiolink::CRC_BYTES];
  const size_t size = audiolink::encode(frame, Type::Ping, nextSeq, nullptr, 0);
  nextSeq = static_cast<uint8_t>(nextSeq + 1);
  clearWindowLocked();
  pingOutstanding = true;
  lastPingMs = millis();
  UsbSerialHost::write(frame, size, WRITE_TIMEOUT_MS);
}

void resendWindowLocked() {
  for (size_t i = 0; i < windowCount; ++i) {
    const Slot& slot = window[(windowHead + i) % audiolink::MAX_WINDOW_FRAMES];
    UsbSerialHost::write(slot.frame, slot.size, WRITE_TIMEOUT_MS);
  }
  ++linkCounters.resends;
  lastProgressMs = millis();
}

void handleAck(const uint8_t* payload, const uint16_t length) {
  if (length < sizeof(audiolink::AckPayload)) return;
  audiolink::AckPayload ack;
  std::memcpy(&ack, payload, sizeof(ack));
  Guard guard;
  bool advanced = false;
  while (windowCount > 0 && audiolink::seqBefore(window[windowHead].seq, ack.nextSeq)) {
    inflightAudio -= window[windowHead].audioBytes;
    inflightBytes -= window[windowHead].size;
    windowHead = (windowHead + 1) % audiolink::MAX_WINDOW_FRAMES;
    --windowCount;
    advanced = true;
  }
  rxFree = ack.rxFree;
  if (advanced || windowCount == 0) lastProgressMs = millis();
  const bool inWindow =
      windowCount == 0 || static_cast<uint8_t>(ack.nextSeq - window[windowHead].seq) <= windowCount;
  if ((ack.flags & audiolink::ACK_OUT_OF_SEQUENCE) && !inWindow) {
    // The module expects a sequence number we never sent or already dropped: the two ends are out
    // of step. Resending cannot fix that; start a new session.
    AudioModule::logf("Link: sequence out of step (module wants %u), reconnecting", ack.nextSeq);
    resetSessionLocked();
    xSemaphoreGive(progress);
    return;
  }
  if ((ack.flags & audiolink::ACK_OUT_OF_SEQUENCE) && windowCount > 0 &&
      millis() - lastFastResendMs >= audiolink::FAST_RESEND_GAP_MS) {
    // The module saw a gap: something in the window was lost. Resend it now rather than at the timeout.
    lastFastResendMs = millis();
    resendWindowLocked();
  }
  xSemaphoreGive(progress);
}

void handleHello(const uint8_t* payload, const uint16_t length) {
  Guard guard;
  if (length >= sizeof(helloPayload)) std::memcpy(&helloPayload, payload, sizeof(helloPayload));
  if (pingOutstanding && length >= sizeof(helloPayload)) {
    pingOutstanding = false;
    // Continue from where the module is, which is not necessarily after our PING (see HelloPayload).
    nextSeq = helloPayload.nextSeq;
    clearWindowLocked();
    linkReady = true;
    ++session;
    AudioModule::logf("Link: module %u.%u ready (protocol %u)", helloPayload.firmwareMajor,
                      helloPayload.firmwareMinor, helloPayload.protocolVersion);
  } else {
    // Nobody asked: the module rebooted. Start a new session; the player restarts its stream.
    AudioModule::logf("Link: module restarted");
    resetSessionLocked();
  }
}

void handleBtDevice(const uint8_t* payload, const uint16_t length) {
  if (length < sizeof(audiolink::BtDevicePayload)) return;
  audiolink::BtDevicePayload found;
  std::memcpy(&found, payload, sizeof(found));
  found.name[sizeof(found.name) - 1] = '\0';
  Guard guard;
  const auto known = std::find_if(devices.begin(), devices.end(), [&found](const BtDevice& device) {
    return std::memcmp(device.address, found.address, 6) == 0;
  });
  if (known != devices.end()) {
    known->rssi = found.rssi;
    if (found.name[0]) known->name = found.name;
    return;
  }
  if (devices.size() >= MAX_BT_DEVICES) return;
  BtDevice device;
  std::memcpy(device.address, found.address, 6);
  device.rssi = found.rssi;
  device.name = found.name;
  devices.push_back(device);
}

void handleFrame() {
  const uint8_t* payload = parser->payload();
  const uint16_t length = parser->length();
  switch (parser->type()) {
    case Type::Ack:
      handleAck(payload, length);
      break;
    case Type::Hello:
      handleHello(payload, length);
      break;
    case Type::Status:
      if (length >= sizeof(audiolink::StatusPayload)) {
        Guard guard;
        std::memcpy(&lastStatus, payload, sizeof(lastStatus));
        lastStatusMs = millis();
        statusValid = true;
      }
      break;
    case Type::BtDevice:
      handleBtDevice(payload, length);
      break;
    case Type::Log:
      AudioModule::logf("ATOM: %.*s", static_cast<int>(length), reinterpret_cast<const char*>(payload));
      break;
    default:
      break;
  }
}

void resendIfStalled() {
  Guard guard;
  if (windowCount == 0 || millis() - lastProgressMs < audiolink::RESEND_TIMEOUT_MS) return;
  resendWindowLocked();
}

void linkTask(void*) {
  uint8_t chunk[READ_CHUNK];
  for (;;) {
    if (suspended || !UsbSerialHost::isConnected()) {
      {
        Guard guard;
        resetSessionLocked();
      }
      vTaskDelay(pdMS_TO_TICKS(50));
      continue;
    }
    {
      Guard guard;
      if (!linkReady && millis() - lastPingMs >= PING_INTERVAL_MS) sendPingLocked();
    }
    const size_t got = UsbSerialHost::read(chunk, sizeof(chunk), 10);
    for (size_t i = 0; i < got; ++i) {
      if (parser->push(chunk[i])) handleFrame();
    }
    linkCounters.badFrames = parser->badFrames();
    resendIfStalled();
  }
}

bool trySendLocked(const Type type, const uint8_t* prefix, const size_t prefixLength, const uint8_t* payload,
                   const size_t length, const uint16_t audioBytes) {
  if (!linkReady || windowCount >= audiolink::MAX_WINDOW_FRAMES) return false;
  const size_t frameSize = audiolink::HEADER_BYTES + prefixLength + length + audiolink::CRC_BYTES;
  if (windowCount > 0 && inflightBytes + frameSize > audiolink::MAX_INFLIGHT_BYTES) return false;
  if (audioBytes > 0 && inflightAudio + audioBytes > rxFree) return false;
  Slot& slot = window[(windowHead + windowCount) % audiolink::MAX_WINDOW_FRAMES];
  uint8_t body[audiolink::MAX_PAYLOAD];
  std::memcpy(body, prefix, prefixLength);
  if (length > 0) std::memcpy(body + prefixLength, payload, length);
  slot.seq = nextSeq;
  slot.size = static_cast<uint16_t>(audiolink::encode(slot.frame, type, nextSeq, body, prefixLength + length));
  slot.audioBytes = audioBytes;
  nextSeq = static_cast<uint8_t>(nextSeq + 1);
  if (windowCount == 0) lastProgressMs = millis();
  ++windowCount;
  inflightAudio += audioBytes;
  inflightBytes += slot.size;
  ++linkCounters.framesSent;
  linkCounters.audioBytes += audioBytes;
  UsbSerialHost::write(slot.frame, slot.size, WRITE_TIMEOUT_MS);
  return true;
}

bool sendWithPrefix(const Type type, const uint8_t* prefix, const size_t prefixLength, const uint8_t* payload,
                    const size_t length, const uint16_t audioBytes, const uint32_t timeoutMs) {
  if (!lock || prefixLength + length > audiolink::MAX_PAYLOAD) return false;
  const uint32_t start = millis();
  for (;;) {
    {
      Guard guard;
      if (!linkReady) return false;
      if (trySendLocked(type, prefix, prefixLength, payload, length, audioBytes)) return true;
    }
    const uint32_t waited = millis() - start;
    if (waited >= timeoutMs) return false;
    xSemaphoreTake(progress, pdMS_TO_TICKS(std::min<uint32_t>(20, timeoutMs - waited)));
  }
}

}  // namespace

void begin() {
  if (task) return;
  lock = xSemaphoreCreateMutex();
  progress = xSemaphoreCreateBinary();
  window = static_cast<Slot*>(
      heap_caps_malloc(sizeof(Slot) * audiolink::MAX_WINDOW_FRAMES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  parser = new audiolink::Parser();
  if (!lock || !progress || !window || !parser) {
    AudioModule::logf("Link: out of memory");
    return;
  }
  xTaskCreate(linkTask, "audio_link", TASK_STACK, nullptr, TASK_PRIORITY, &task);
}

void setSuspended(const bool value) { suspended = value; }

bool ready() {
  if (!lock) return false;
  Guard guard;
  return linkReady;
}

audiolink::HelloPayload hello() {
  Guard guard;
  return helloPayload;
}

uint32_t sessionCounter() {
  if (!lock) return 0;
  Guard guard;
  return session;
}

ModuleStatus status() {
  ModuleStatus out;
  if (!lock) return out;
  Guard guard;
  out.valid = statusValid && linkReady;
  out.ageMs = millis() - lastStatusMs;
  out.status = lastStatus;
  return out;
}

std::vector<BtDevice> btDevices() {
  if (!lock) return {};
  Guard guard;
  return devices;
}

void clearBtDevices() {
  if (!lock) return;
  Guard guard;
  devices.clear();
}

bool send(const Type type, const void* payload, const size_t length, const uint32_t timeoutMs) {
  return sendWithPrefix(type, nullptr, 0, static_cast<const uint8_t*>(payload), length, 0, timeoutMs);
}

bool sendAudio(const uint8_t streamId, const uint8_t* data, const size_t length, const uint32_t timeoutMs) {
  return sendWithPrefix(Type::AudioData, &streamId, 1, data, length, static_cast<uint16_t>(length + 1), timeoutMs);
}

Counters counters() {
  if (!lock) return {};
  Guard guard;
  return linkCounters;
}

}  // namespace AudioLink

#endif  // INX_HAS_AUDIO_MODULE
