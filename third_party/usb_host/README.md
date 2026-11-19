# Vendored ESP-IDF USB Host class drivers

Unmodified copies of Espressif's ESP-IDF Component Registry packages, used only by the
`t5s3_audio` env (see `platformio.ini`) to talk to the external audio module's USB-to-UART bridge, and
to USB headphone adapters (UAC 1.0, `src/system/UsbAudioOut.h`):

| Folder | Registry package | Version |
| --- | --- | --- |
| `usb_host_cdc_acm` | `espressif/usb_host_cdc_acm` | 2.4.1 |
| `usb_host_ch34x_vcp` | `espressif/usb_host_ch34x_vcp` | 2.2.1 |
| `usb_host_cp210x_vcp` | `espressif/usb_host_cp210x_vcp` | 2.2.0 |
| `usb_host_ftdi_vcp` | `espressif/usb_host_ftdi_vcp` | 2.1.1 |
| `usb_host_uac` | `espressif/usb_host_uac` | 1.5.0 |
| `esp_idf_usb` | ESP-IDF `components/usb` (USB Host Library) | 5.5.2, commit 87912cd291 |

Each package keeps its Apache-2.0 `LICENSE`. Only the test and example folders were dropped and a
PlatformIO `library.json` was added. `usb_host_uac`'s Kconfig options are set as `-DCONFIG_UAC_*`
build flags in the env, and its version macros (normally from `cmake_utilities`) in its
`library.json`. To update, download the new archives from components.espressif.com and replace the
folders, keeping the `library.json` files. The USB Host Library itself (`usb/usb_host.h`) is built
from source too: see `esp_idf_usb` below.

## `esp_idf_usb`: the USB Host Library itself

arduino-esp32 3.3.x ships the USB Host Library prebuilt (`libusb.a`) with
`CONFIG_USB_HOST_CONTROL_TRANSFER_MAX_SIZE=256`. Enumeration then fails on any device whose
configuration descriptor is longer ("Configuration descriptor larger than control transfer max
length"), which includes USB audio adapters: the Sabrent AU-UCMA's is 519 bytes. `esp_idf_usb`
is the same library compiled from source with the limit at 1024. All its objects are linked
from here, so no member of the prebuilt `libusb.a` is used (check `firmware.map` for `libusb.a(`
after an update).

- Sources: `components/usb` of ESP-IDF at the commit named in the framework's `versions.txt`
  (`framework-arduinoespressif32-libs`), minus tests. The public headers are not copied: they are
  identical to the framework's `include/usb/include`, which the build already uses. Diff them when
  updating; if they differ, the framework and this copy are out of step.
- `LICENSE` is ESP-IDF's (Apache-2.0).
- Not from ESP-IDF: `inx_usb_config.h` (the redefined option) and `inx_usb_config.py` (the
  library's extraScript that force-includes it), plus `library.json`.
