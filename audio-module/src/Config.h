#pragma once

/**
 * @file Config.h
 * @brief Pins, buffer sizes and task layout for the ATOM Lite audio module.
 */

#include <cstddef>
#include <cstdint>

// PCM5102A I2S DAC on the ATOM Lite's header. Its SCK pin goes to GND (internal PLL), XSMT high.
#ifndef AUDIO_MODULE_I2S_BCK
#define AUDIO_MODULE_I2S_BCK 22
#endif
#ifndef AUDIO_MODULE_I2S_WS
#define AUDIO_MODULE_I2S_WS 19
#endif
#ifndef AUDIO_MODULE_I2S_DOUT
#define AUDIO_MODULE_I2S_DOUT 23
#endif

namespace config {

constexpr uint8_t FIRMWARE_MAJOR = 0;
constexpr uint8_t FIRMWARE_MINOR = 1;

// RAM is the constraint: Bluetooth Classic takes ~50 KB of heap and the AAC decoder ~30-60 KB, on
// an ESP32 without PSRAM. The T5 holds a large queue, so the module only needs to ride out jitter.
/** Compressed audio waiting to be decoded: about 2 s at 64 kbit/s. */
constexpr size_t RX_BUFFER_BYTES = 16 * 1024;
/** Decoded stereo frames waiting for the output: about 46 ms at 44.1 kHz. */
constexpr size_t PCM_RING_FRAMES = 2048;
/** Playback starts once the PCM ring is this full, so the first frames do not underrun. */
constexpr size_t PCM_PRIME_FRAMES = PCM_RING_FRAMES / 2;

constexpr size_t UART_RX_BUFFER = 4096;
constexpr size_t UART_TX_BUFFER = 2048;
constexpr uint8_t UART_RX_FIFO_FULL = 32;

/** A2DP sources always send 44.1 kHz stereo (SBC). */
constexpr uint32_t BT_SAMPLE_RATE = 44100;
constexpr const char* BT_LOCAL_NAME = "inx audio";

constexpr int LED_PIN = 27;
constexpr uint8_t LED_LEVEL = 20;

// Tasks. The Bluetooth stack runs on core 0; ours share core 1. The link outranks the decoder so
// the UART never backs up while a frame decodes.
constexpr uint32_t LINK_TASK_STACK = 4096;
constexpr uint32_t DECODE_TASK_STACK = 8192;
constexpr uint32_t OUTPUT_TASK_STACK = 4096;
constexpr int LINK_TASK_PRIORITY = 5;
constexpr int DECODE_TASK_PRIORITY = 3;
constexpr int OUTPUT_TASK_PRIORITY = 4;
constexpr int APP_CORE = 1;

}  // namespace config
