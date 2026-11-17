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
 * USB audio adapters are the usual case: the Sabrent AU-UCMA's is 519 bytes. 2048 covers anything
 * the T5 is likely to see; it sizes one control transfer buffer per client and for enumeration.
 */
#undef CONFIG_USB_HOST_CONTROL_TRANSFER_MAX_SIZE
#define CONFIG_USB_HOST_CONTROL_TRANSFER_MAX_SIZE 2048
