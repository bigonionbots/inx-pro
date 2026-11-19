/**
 * @file inx_usb_config.h
 * @brief Forced include (library.json) for the vendored ESP-IDF USB Host Library: the one setting
 *        that differs from the prebuilt framework's sdkconfig.
 *
 * Not part of ESP-IDF. sdkconfig.h has `#pragma once`, so including it here first and redefining
 * the option afterwards wins over the later includes in the library sources.
 */
#pragma once

#include "sdkconfig.h"

/**
 * The prebuilt library allows 256 bytes, so enumeration fails on devices with a larger
 * configuration descriptor ("Configuration descriptor larger than control transfer max length").
 * USB audio adapters are the usual case: the Sabrent AU-UCMA's is 519 bytes. It sizes several
 * buffers in DMA-capable internal RAM (enumeration, the hub driver, each opened device), which
 * HTTPS also needs, so it is no larger than simple adapters need: 1024.
 */
#undef CONFIG_USB_HOST_CONTROL_TRANSFER_MAX_SIZE
#define CONFIG_USB_HOST_CONTROL_TRANSFER_MAX_SIZE 1024
