#pragma once

/**
 * @file UsbAudioSink.h
 * @brief Plays the audio module's streams on USB headphones instead: the module's job done on
 *        the T5 (LilyGo T5 S3, t5s3_audio only).
 *
 * It takes the same host frames the module does (AudioLinkProtocol.h: STREAM_START, AUDIO_DATA,
 * STREAM_END, STREAM_STOP, PAUSE, RESUME, SET_VOLUME) and answers with the same STATUS, so
 * AudioPlayer drives it unchanged through AudioOutput.h. A task decodes the compressed stream
 * (PcmDecoder: MP3, AAC-LC, HE-AAC), converts the rate when the adapter does not take the
 * source's (Resampler), and writes to the adapter (UsbAudioOut), whose buffer paces it. playedMs
 * counts source time handed to the adapter, as the module counts what it hands its output.
 *
 * Bluetooth and output selection do not apply; STATUS reports output USB_HEADPHONES (a value
 * outside the protocol's Output enum) and BtState::Off. The volume is the adapter's own (0-100
 * across its range, 0 mutes); it is kept on the SD card by service().
 */

#if INX_HAS_AUDIO_MODULE

#include <AudioLinkProtocol.h>

#include <cstddef>
#include <cstdint>

namespace UsbAudioSink {

/** STATUS output while the headphones play (not an audiolink::Output value). */
constexpr audiolink::Output USB_HEADPHONES = static_cast<audiolink::Output>(0x40);

/** Buffers and the decode task (once, at boot). Reads the saved volume (main loop: SD). */
void begin();

/** An adapter is open and can play. */
bool ready();
/** Changes whenever an adapter is opened, like AudioLink::sessionCounter() for the module. */
uint32_t sessionCounter();

/** A host frame, as AudioLink::send() would send it to the module. */
bool send(audiolink::Type type, const void* payload, size_t length, uint32_t timeoutMs);
/** Compressed audio for streamId, waiting up to timeoutMs for room, as AudioLink::sendAudio(). */
bool sendAudio(uint8_t streamId, const uint8_t* data, size_t length, uint32_t timeoutMs);

/** What the module would report in STATUS. */
audiolink::StatusPayload status();

/** Main loop: saves a changed volume to the SD card. */
void service();

}  // namespace UsbAudioSink

#endif  // INX_HAS_AUDIO_MODULE
