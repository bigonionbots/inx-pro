/**
 * @file AudioOutput.cpp
 * @brief Definitions for AudioOutput.
 */

#include "AudioOutput.h"

#if INX_HAS_AUDIO_MODULE

#include <atomic>

#include "UsbAudioSink.h"

namespace AudioOutput {

namespace {

std::atomic<bool> lastWasHeadphones{false};
std::atomic<uint32_t> switches{0};

}  // namespace

bool headphones() { return UsbAudioSink::ready(); }

bool ready() { return headphones() || AudioLink::ready(); }

uint32_t sessionCounter() {
  const bool now = headphones();
  if (lastWasHeadphones.exchange(now) != now) ++switches;
  // Each part only grows, so the sum changes whenever any of them does.
  return AudioLink::sessionCounter() + UsbAudioSink::sessionCounter() + switches.load();
}

bool send(const audiolink::Type type, const void* payload, const size_t length, const uint32_t timeoutMs) {
  if (headphones()) return UsbAudioSink::send(type, payload, length, timeoutMs);
  return AudioLink::send(type, payload, length, timeoutMs);
}

bool sendAudio(const uint8_t streamId, const uint8_t* data, const size_t length, const uint32_t timeoutMs) {
  if (headphones()) return UsbAudioSink::sendAudio(streamId, data, length, timeoutMs);
  return AudioLink::sendAudio(streamId, data, length, timeoutMs);
}

AudioLink::ModuleStatus status() {
  if (headphones()) {
    AudioLink::ModuleStatus out;
    out.valid = true;
    out.status = UsbAudioSink::status();
    return out;
  }
  return AudioLink::status();
}

}  // namespace AudioOutput

#endif  // INX_HAS_AUDIO_MODULE
