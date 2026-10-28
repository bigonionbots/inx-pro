# Vendored ESP-IDF USB Host class drivers

Unmodified copies of Espressif's ESP-IDF Component Registry packages, used only by the
`t5s3_audio` env (see `platformio.ini`) to talk to the external audio module's USB-to-UART bridge:

| Folder | Registry package | Version |
| --- | --- | --- |
| `usb_host_cdc_acm` | `espressif/usb_host_cdc_acm` | 2.4.1 |
| `usb_host_ch34x_vcp` | `espressif/usb_host_ch34x_vcp` | 2.2.1 |
| `usb_host_cp210x_vcp` | `espressif/usb_host_cp210x_vcp` | 2.2.0 |
| `usb_host_ftdi_vcp` | `espressif/usb_host_ftdi_vcp` | 2.1.1 |

Each package keeps its Apache-2.0 `LICENSE`. Only the test and example folders were dropped and a
PlatformIO `library.json` was added. The USB Host Library itself (`usb/usb_host.h`) comes prebuilt
with arduino-esp32 3.3.x. To update, download the new archives from components.espressif.com and
replace the folders, keeping the `library.json` files.
