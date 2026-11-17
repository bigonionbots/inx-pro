#pragma once

/**
 * @file AudioOutput.h
 * @brief Where AudioPlayer's streams go: the audio module (AudioLink) or USB headphones
 *        (UsbAudioSink), behind the module's own calls (LilyGo T5 S3, t5s3_audio only).
 *
 * USB headphones win when an adapter is open: the port carries one or the other. Switching
 * between them counts as a new session (sessionCounter()), so the player restarts its stream on
 * the new output at the position it had reached.
 */

#if INX_HAS_AUDIO_MODULE

#include <AudioLinkProtocol.h>

#include <cstddef>
#include <cstdint>

#include "AudioLink.h"

namespace AudioOutput {

/** USB headphones are the output (an adapter is open and can play). */
bool headphones();

/** The current output takes commands and audio. */
bool ready();
/** Changes when the output reconnects, restarts, or switches between module and headphones. */
uint32_t sessionCounter();

bool send(audiolink::Type type, const void* payload, size_t length, uint32_t timeoutMs);
bool sendAudio(uint8_t streamId, const uint8_t* data, size_t length, uint32_t timeoutMs);
AudioLink::ModuleStatus status();

}  // namespace AudioOutput

#endif  // INX_HAS_AUDIO_MODULE
