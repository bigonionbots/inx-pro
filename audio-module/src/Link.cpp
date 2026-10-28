/**
 * @file Link.cpp
 * @brief Definitions for the module end of the T5 link.
 */

#include "Link.h"

#include <Arduino.h>
#include <AudioLinkProtocol.h>
#include <Preferences.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "Config.h"
#include "Outputs.h"
#include "Player.h"

namespace hostlink {

namespace {

using audiolink::Type;

// Cumulative, so a lost ACK costs nothing; batching keeps the module->host direction light.
constexpr uint32_t ACK_INTERVAL_MS = 10;
// A host that ran out of credit sends nothing, so it would never hear that space freed up:
// re-advertise rxFree on its own when it changed.
constexpr uint32_t CREDIT_UPDATE_MS = 50;
constexpr size_t READ_CHUNK = 256;
constexpr int FLUSH_WAIT_MS = 20;

SemaphoreHandle_t txLock = nullptr;
audiolink::Parser parser;
uint8_t expectedSeq = 0;
int linkStream = -1;  // last STREAM_START id, -1 after STREAM_STOP
bool ackPending = false;
bool ackUrgent = false;
bool outOfSequence = false;
uint32_t lastAckMs = 0;
uint32_t lastAckRxFree = 0;
uint32_t lastStatusMs = 0;

void send(const Type type, const void* payload, const size_t length) {
  uint8_t frame[audiolink::HEADER_BYTES + 128 + audiolink::CRC_BYTES];
  if (length > 128) return;
  const size_t size = audiolink::encode(frame, type, 0, payload, length);
  xSemaphoreTake(txLock, portMAX_DELAY);
  Serial.write(frame, size);
  xSemaphoreGive(txLock);
}

void sendHello() {
  audiolink::HelloPayload hello{};
  hello.protocolVersion = audiolink::PROTOCOL_VERSION;
  hello.firmwareMajor = config::FIRMWARE_MAJOR;
  hello.firmwareMinor = config::FIRMWARE_MINOR;
  hello.outputs = outputs::supported();
  hello.rxBufferBytes = config::RX_BUFFER_BYTES;
  hello.nextSeq = expectedSeq;
  send(Type::Hello, &hello, sizeof(hello));
}

void sendAck() {
  audiolink::AckPayload ack{};
  ack.nextSeq = expectedSeq;
  ack.rxFree = static_cast<uint32_t>(player::rxFree());
  ack.flags = outOfSequence ? audiolink::ACK_OUT_OF_SEQUENCE : 0;
  outOfSequence = false;
  lastAckRxFree = ack.rxFree;
  send(Type::Ack, &ack, sizeof(ack));
  ackPending = false;
  ackUrgent = false;
  lastAckMs = millis();
}

void sendStatus() {
  audiolink::StatusPayload status{};
  player::fillStatus(status);
  status.output = static_cast<uint8_t>(outputs::current());
  status.btState = static_cast<uint8_t>(outputs::btState());
  outputs::btAddress(status.btAddress);
  send(Type::Status, &status, sizeof(status));
  lastStatusMs = millis();
}

void saveSetting(const char* key, const uint8_t value) {
  Preferences prefs;
  if (prefs.begin("audio", false)) {
    prefs.putUChar(key, value);
    prefs.end();
  }
}

/** Handles an in-sequence host frame. False leaves it unaccepted so the T5 resends it later. */
bool handleHostFrame(const Type type, const uint8_t* payload, const uint16_t length) {
  switch (type) {
    case Type::AudioData: {
      if (length < 1) return true;
      const int streamId = payload[0];
      if (streamId != linkStream) return true;  // a stream that was stopped or replaced: drop it
      // Right after STREAM_START the decoder task (lower priority) still has to reset its buffers.
      // Yield to it briefly rather than refusing, which would cost the host a resend timeout.
      for (int i = 0; i < FLUSH_WAIT_MS && player::acceptingStream() != streamId; ++i) vTaskDelay(1);
      if (player::acceptingStream() != streamId) return false;
      return player::pushData(payload + 1, length - 1);
    }
    case Type::StreamStart: {
      if (length < sizeof(audiolink::StreamStartPayload)) return true;
      audiolink::StreamStartPayload start;
      std::memcpy(&start, payload, sizeof(start));
      linkStream = start.streamId;
      player::streamStart(start.streamId, static_cast<audiolink::Codec>(start.codec), start.startMs);
      return true;
    }
    case Type::StreamEnd:
      if (length >= 1) player::streamEnd(payload[0]);
      return true;
    case Type::StreamStop:
      if (length >= 1 && payload[0] == linkStream) {
        player::streamStop(payload[0]);
        linkStream = -1;
      }
      return true;
    case Type::Pause:
      player::pause();
      return true;
    case Type::Resume:
      player::resume();
      return true;
    case Type::SetVolume:
      if (length >= 1) {
        player::setVolume(payload[0]);
        saveSetting("volume", payload[0]);
      }
      return true;
    case Type::SetOutput:
      if (length >= 1) {
        outputs::select(static_cast<audiolink::Output>(payload[0]));
        saveSetting("output", payload[0]);
      }
      return true;
    case Type::BtScan:
      outputs::btScan(length >= 1 && payload[0] != 0);
      return true;
    case Type::BtConnect:
      if (length >= 6) outputs::btConnect(payload);
      return true;
    case Type::BtDisconnect:
      outputs::btDisconnect();
      return true;
    case Type::BtForget:
      outputs::btForget();
      return true;
    default:
      return true;  // unknown command: accept and ignore, so a newer T5 cannot stall the link
  }
}

void handleFrame() {
  const Type type = parser.type();
  if (static_cast<uint8_t>(type) >= 0x80) return;  // module->host types echoed back: ignore

  if (type == Type::Ping) {
    expectedSeq = static_cast<uint8_t>(parser.seq() + 1);
    sendHello();
    sendAck();
    return;
  }
  if (parser.seq() != expectedSeq) {
    // A gap means a frame was lost or damaged: ask for a resend from expectedSeq. A duplicate
    // (an older sequence number) is the host already resending, so just acknowledge.
    if (!audiolink::seqBefore(parser.seq(), expectedSeq)) outOfSequence = true;
    ackPending = true;
    ackUrgent = true;
    return;
  }
  if (handleHostFrame(type, parser.payload(), parser.length())) {
    ++expectedSeq;
  } else {
    ackUrgent = true;
  }
  ackPending = true;
}

void linkTask(void*) {
  uint8_t chunk[READ_CHUNK];
  sendHello();
  for (;;) {
    const int available = Serial.available();
    if (available > 0) {
      const size_t n = Serial.readBytes(chunk, std::min(static_cast<size_t>(available), sizeof(chunk)));
      for (size_t i = 0; i < n; ++i) {
        if (parser.push(chunk[i])) handleFrame();
      }
    }
    const uint32_t now = millis();
    if (ackPending && (ackUrgent || now - lastAckMs >= ACK_INTERVAL_MS)) {
      sendAck();
    } else if (now - lastAckMs >= CREDIT_UPDATE_MS && player::rxFree() != lastAckRxFree) {
      sendAck();
    }
    if (now - lastStatusMs >= audiolink::STATUS_PERIOD_MS) sendStatus();
    if (available <= 0) vTaskDelay(1);
  }
}

}  // namespace

bool begin() {
  txLock = xSemaphoreCreateMutex();
  Serial.setRxBufferSize(config::UART_RX_BUFFER);
  Serial.setTxBufferSize(config::UART_TX_BUFFER);
  Serial.begin(audiolink::BAUD, SERIAL_8N1, -1, -1, false, 20000UL, config::UART_RX_FIFO_FULL);
  return txLock && xTaskCreatePinnedToCore(linkTask, "link", config::LINK_TASK_STACK, nullptr,
                                           config::LINK_TASK_PRIORITY, nullptr, config::APP_CORE) == pdPASS;
}

void log(const char* format, ...) {
  char text[120];
  va_list args;
  va_start(args, format);
  const int length = vsnprintf(text, sizeof(text), format, args);
  va_end(args);
  if (length > 0) send(Type::Log, text, std::min(static_cast<size_t>(length), sizeof(text) - 1));
}

void sendBtDevice(const uint8_t address[6], const int8_t rssi, const char* name) {
  audiolink::BtDevicePayload device{};
  std::memcpy(device.address, address, sizeof(device.address));
  device.rssi = rssi;
  std::strncpy(device.name, name, sizeof(device.name) - 1);
  send(Type::BtDevice, &device, sizeof(device));
}

}  // namespace hostlink
