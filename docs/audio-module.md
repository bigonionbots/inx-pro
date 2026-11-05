# External USB-C audio module (LilyGo T5 S3)

An add-on for the `t5s3` firmware. A small module, an M5Stack ATOM Lite (ESP32-PICO-D4) plus an
optional PCM5102A I2S DAC, plugs into the T5's only USB-C port. One cable carries power and data:
the T5 powers the module from its charger's OTG boost and talks to the module's USB-to-UART
bridge as a USB host. Later stages play AudioBookShelf audiobooks through it, over Bluetooth A2DP
or wired.

Build the `t5s3_audio` env. The plain `t5s3` env does not contain any of this.

## Hardware findings

From `hardware/T5 E-paper S3 Pro V1.0 24-12-24.pdf` in Xinyuan-LilyGO/T5S3-4.7-e-paper-PRO. The
Pro Lite uses the same board without LoRa and GPS.

- **VBUS is the charger's BUS pin.** The USB-C connector's VBUS (A4/A9/B4/B9) goes straight to the
  BQ25896's BUS pin with only a TVS (ED10) in between, with no load switch. When the boost runs, it
  drives the port.
- **Only a register controls the boost.** The BQ25896 OTG pin is pulled up to VSYS through R25
  (10k), so boost = REG03 `OTG_CONFIG`. That bit **resets to 1**, and the I2C watchdog (40 s by
  default) restores every register to its default when it expires. Firmware that only clears
  `OTG_CONFIG` gets the boost back 40 s later. `Bq25896::setOtgConfig()` disables the watchdog
  first. To check on hardware: stock `t5s3` firmware never writes the charger, so the boost may
  be running on battery whenever nothing is plugged in. The first line of the module log shows
  REG03 / VBUS_STAT as found at boot.
- **CC pins**: 5.1k pull-downs on CC1 (R6) and CC2 (R71), so the port is sink-only. No PD or
  source controller is involved, so a plain C-to-C cable to the ATOM works once the boost is on.
- **D+/D-** go to GPIO19/20 (native USB) through 10R series resistors (R1/R2) and ULC0511C ESD diodes.
- **Spare console**: UART0 TXD0/RXD0 (GPIO43/44) go to test pads TP3/TP4 and, through 0R
  links (R11/R12), to the GPS header that the Lite leaves unpopulated. A 3.3 V USB-UART adapter on TP3/TP4 gives a
  console while the USB port is in host mode. Optional: the Audio Module screen shows the log anyway.
- **Battery capacity**: not on the schematic (battery connector P2). The Audio Module screen
  shows the BQ27220's design and learned full-charge capacity, plus average current, which also
  gives the module's real draw.

## The ATOM Lite's USB bridge (measured 2026-10-28)

The test unit's USB serial is **not an FTDI chip**. It enumerates as `0403:6001` with manufacturer
"Hades2001", product "M5stack" and bcdDevice 5.00 (it claims to be an FT2232C), so it is an
MCU-based FTDI emulation. Measured from a Linux PC (`audio-module/tools/`, kernel `ftdi_sio`) and from the T5 (Audio
Module -> Link test):

| Test | Result |
| --- | --- |
| Line rates | 921600 and 1M come out garbled (the module transmits at the wrong rate as seen by the bridge). 500k, 750k and 1.5M are bit-clean. 115200 works but uses the ESP32's 1 MHz REF_TICK clock; 230400 on REF_TICK is too coarse to work. |
| Host -> module | Reliable at every clean rate: 0 errors, 10-17 KB/s (the bridge, not the line rate, sets the ceiling). |
| Module -> host | Drops whole 8-byte blocks, and reports no overrun. Exact only when trickled: 16-byte bursts every 4 ms, about 3 KB/s. A continuous stream loses 40-70 % at any line rate, even 115200. |
| **From the T5** (ESP-IDF FTDI driver, 500k) | Host -> module 8192/8192, 0 bad, 211 ms (about 39 KB/s). Module -> host 4096/4096 both paced and unpaced, 0 drops, 0 line errors. |

Consequences:
- The link runs at **500000** baud (`INX_AUDIO_MODULE_BAUD`, `AUDIO_MODULE_BAUD`).
- The module -> host losses show up only with the Linux host. The T5's ESP-IDF host polls the
  bridge differently and receives everything. PC tests therefore understate the link, so
  judge the bridge from the T5.
- Even so: there is no hardware flow control, and the bridge reports no overruns, so the
  protocol still needs credits, CRC and retransmission.
- Modules with a real CH9102F / CP2104 / CH340 bridge use the same T5 code.

## What the firmware does now (stages 1–3)

- `src/system/Bq25896.*`: charger status, VBUS ADC, and the `OTG_CONFIG`/watchdog switch.
- `src/system/UsbSerialHost.*`: the ESP-IDF USB Host Library (prebuilt in arduino-esp32 3.3.7)
  plus the class drivers vendored in `third_party/usb_host/` (CDC-ACM 2.4.1, FTDI, CP210x and CH34x
  VCP). The driver is picked by VID: FTDI `0403`, CP210x `10C4`, CH340/341 `1A86:7522/7523/5523`.
  Everything else, including the CH9102F `1A86:55D4`, uses the generic CDC-ACM driver. The link
  runs 8N1 at `INX_AUDIO_MODULE_BAUD` (500000) and keeps DTR and RTS deasserted, so the ATOM's
  auto-reset circuit never resets it or drops it into its bootloader. Unplugging and replugging
  are handled.
- `src/system/AudioModule.*`: power policy and the bench tools.
  - At boot the boost is forced off.
  - The boost is refused while anything else powers VBUS.
  - It switches off after 20 s with no bridge connected, on a boost fault, and before deep sleep.
  - The link works without the boost, for a module that has its own battery.
- `Device Management -> Audio Module` (`AudioModuleActivity`) has these controls:
  - Module power
  - USB link
  - Link test: host -> module 8 KB checked by the module; module -> host 4 KB trickled and 4 KB
    unpaced. Needs the `atom_linkdiag` module firmware.

  It shows charger/VBUS, battery, bridge VID:PID and driver, traffic counters and the log.

**USB console.** The PHY is shared. Until you switch the USB link on, the USB-Serial-JTAG
console and esptool work exactly as in `t5s3`. After that they are gone until reboot. To flash
anyway: hold BOOT, tap RST, release BOOT.

## Bench test (stage 2)

1. Flash the ATOM from `audio-module/`: `cd audio-module && pio run -t upload`. Its LED turns
   blue. Optional: before unplugging it, note the bridge with `lsusb` (VID:PID).
2. Flash the T5: `pio run -e t5s3_audio -t upload`, then unplug the PC.
3. Plug the ATOM into the T5 with a C-to-C cable, then open Device Management -> Audio Module.
4. Tap **Module power**. Expected: VBUS shows `OTG boost` at about 5.1 V and the ATOM's LED
   lights up.
5. Tap **USB link**. Expected: BRIDGE shows `VID:PID driver product`.
6. Tap **Link test**. With a good bridge both rows show every byte with 0 bad and "exact".
   With this ATOM Lite's bridge, expect T5 -> MODULE clean, MODULE -> T5 paced exact, and
   unpaced damaged (see the bridge section).

From a PC instead: `python3 audio-module/tools/link_diag.py /dev/ttyUSB0` with the default
firmware, or `echo_test.py` with `pio run -e atom_echo -t upload`.

Results so far (2026-10-28):
- bridge: emulated FTDI `0403:6001`, see above
- module draw: about 100 mA more from the battery with the boost on and the link up
  (-72 mA -> -170 mA average)
- `OTG_CONFIG` at stock boot: still unknown, because the charger kept the earlier writes. It
  only resets when its power is fully removed.

## Playback (stage 4)

Books play from the T5's SD card, with no Wi-Fi involved. The T5 reads and demuxes the files and streams
the compressed audio over the USB link. The ATOM decodes it and plays it to Bluetooth headphones,
a PCM5102A, or a silent test sink.

**Using it.** Copy books to `/Audiobooks` on the SD card. Each book is one `.m4b` / `.m4a` / `.mp4` /
`.mp3` file, or a folder of them, played in natural name order ("Part 2" before "Part 10").
**Home -> Shortcuts -> Audiobooks** lists them. Pick one and it resumes where you left off;
progress is in `/.system/audio_progress.txt`.
- The **Output** view picks Bluetooth headphones (Scan, tap the headset in pairing mode;
  it is remembered and reconnects), the wired DAC, or the silent test sink.
- Playback continues after closing the screen, so you can listen while reading. The T5 stays awake
  while a book plays. Pause, then sleep, and the module powers off.
- Confirm toggles play/pause and Up/Down change the volume.

**Pieces:**

| Where | What |
| --- | --- |
| `shared/audio_link/AudioLinkProtocol.h` | Wire protocol, shared verbatim by both firmwares: framing with CRC-16, reliable go-back-N host frames, credit flow control on the ACK, STATUS/BT_DEVICE/LOG from the module. |
| `src/audio/` | `Mp3Source` (ID3 skip, Xing/VBRI duration and TOC seek) and `Mp4Source` (first AAC track to ADTS; stsz/stco read from the card on demand, so 20 h books are fine). The ADTS output is byte-identical to `ffmpeg -c:a copy -f adts`. |
| `src/system/AudioLink.*` | T5 end of the protocol: link task, resend window in PSRAM, status cache, Bluetooth scan results. |
| `src/system/AudioPlayer.*` | Books, tracks, progress. SD reads happen only on the main loop, into a 512 KB PSRAM queue, about a minute of audio. A sender task drains it to the link. This avoids racing the reader's own SD access, since SdFat is not thread-safe and most reader I/O does not take SdIoMutex. |
| `src/activity/settings/AudioPlayerActivity.*` | Now playing / Books / Output screens. |
| `audio-module/src/` | `Link` (frames, ACKs, STATUS), `Player` (16 KB input buffer, libhelix MP3/AAC-LC, resampling to 44.1 kHz stereo, volume), `Outputs` (ESP32-A2DP source, I2S, null sink). The output and volume persist in NVS. |

**Live streams (game sound, protocol 3).** Besides MP3 and AAC, a stream may be `Codec::MuLaw`:
G.711 µ-law bytes (`shared/audio_link/MuLaw.h`), with the sample rate and channel count in
STREAM_START (fields added in protocol 3; a protocol 2 host's shorter STREAM_START still works).
The module plays a live stream near real time: it starts once `LIVE_PRIME_MS` (60 ms) is waiting,
drops what arrives while more than `LIVE_MAX_BUFFERED_MS` (150 ms) is, and after running dry
waits for 60 ms again. STATUS `decodeErrors` counts those drops for a live stream. Games send 22050 Hz mono (see
`docs/games.md`); when the output runs at exactly twice the stream rate, as Bluetooth and I2S do
at 44.1 kHz, the module upsamples with `Resample2x.h`'s low-pass instead of its linear resampler. Both firmwares must be updated together for it.

**Module firmware envs** (`audio-module/platformio.ini`):
- `atom_lite`, the default, is the player.
- `atom_linkdiag` is the raw stage-2 link test used by the T5's Audio Module -> Link test.
- `atom_echo`, `atom_diag` and `atom_beacon` are the PC bench tools.

**PC stand-in for the T5:** `audio-module/tools/play.py PORT FILE --output null|bt|i2s`, plus
`--scan N` / `--connect ADDR`. It uses ffmpeg to turn M4B into ADTS.

**PCM5102A wiring (ATOM Lite header):**
- BCK -> G22, LRCK -> G19, DIN -> G23. Override with `AUDIO_MODULE_I2S_*`.
- SCK -> GND (internal PLL), XSMT -> 3V3, FMT/FLT/DEMP -> GND.

**Memory on the ATOM Lite** (ESP32-PICO-D4, no PSRAM; measured 2026-10-28):

| Stage | Free heap |
| --- | --- |
| Boot | 217 KB (largest block 110 KB) |
| After the module's buffers and tasks | 157 KB |
| Bluetooth Classic started | ~75 KB |
| Plus the MP3 or AAC decoder | ~53 KB |

HE-AAC (SBR) would need another 51 KB contiguous block, so it is compiled out (`HELIX_NO_AAC_SBR`, vendored `audio-module/lib/libhelix`). HE-AAC books play their AAC-LC core, which sounds duller. Full HE-AAC needs a module with PSRAM.

**Measured from a PC** (`tools/play.py`, silent output): MP3 22 kHz mono and AAC-LC 44.1 kHz
mono M4B both play in real time, with 0 resends and 0 decode errors. 128 kbit/s stereo starves
from a Linux PC, because the Linux FTDI path tops out around 10 KB/s. The T5's host measured
39 KB/s on the same bridge, so 128 kbit/s still has to be checked on the T5.

**Licences:** ESP32-A2DP is Apache-2.0. codec-helix (libhelix, vendored) is GPL-3.0, with the Helix
decoders under the RealNetworks RPSL. The ATOM firmware binary is therefore GPL-3.0. The T5
firmware does not link either library.

## Audiobookshelf

Books from an Audiobookshelf server can be streamed through the T5 or downloaded to the SD card.
Either way they play through the module exactly like local books. Open it from
**Audiobooks -> Books -> Audiobookshelf**.

**Wi-Fi.** When the screen needs the server and Wi-Fi is down, it opens the Wi-Fi picker (as
the other network screens do) and carries on once connected; cancelling shows "Audiobookshelf
needs Wi-Fi" with a Connect button, and Back gives up. That connection is handed to `AbsWifi`,
which turns Wi-Fi off once nothing uses it (leaving the screen while a stream plays keeps it).
Signing out works offline. Background work with no screen to ask (streaming, progress sync of
downloaded books) connects quietly with the saved networks instead.

**HTTPS memory.** The prebuilt framework gives mbedTLS internal RAM only, and a handshake needs
~40-50 KB (a 16 KB record buffer plus working memory). With the reader running, the largest
internal block was 30 KB, and every HTTPS request failed with "SSL - Memory allocation failed".
`Abs::begin()` therefore points mbedTLS's allocator at PSRAM first (internal as fallback), as
the framework's `CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC` would. A failed connection says why: name
not found (with the DNS server asked), host unreachable, certificate not trusted, or the
mbedTLS error and free internal RAM.

**Home.** Signed in, the screen opens on the library's shelves, as the server's home page has
them (`GET /api/libraries/:id/personalized`): Continue Listening, Continue Series, Recently Added,
Listen Again, a few books each with the listening progress. **All books** is the full list by
title; with more than one library, **Libraries** switches. Back from a book opened on a shelf
reloads them, so Continue Listening is current.

**Sign-in.** The server URL, username and password are entered once. The client uses the v2.26+
JWT scheme, checked against the v2.37.1 server source:
- `POST /login` with `x-return-tokens: true` returns `user.accessToken` and `user.refreshToken`.
- On a 401, `POST /auth/refresh` with `x-refresh-token` returns a new access token and a rotated
  refresh token, and the request is retried once.
- Older servers fall back to the legacy `user.token`.

The password is never stored. The tokens are saved in `/.system/audiobookshelf.json`. A refresh
token unused for 30 days (the server default) means signing in again.

**Streaming.** `HttpAudioFile` reads a track with HTTP Range requests (`GET
/api/items/:id/file/:ino`, served by Express `sendFile`). It keeps three 128 KB blocks in PSRAM:
the audio being played, plus the MP4 sample tables elsewhere in the file. Underneath, the same
`Mp3Source`/`Mp4Source` readers run on a feeder task, so a streamed book needs no SD access.
Network errors are retried. If retries fail, playback restarts from the current position after
5 s. Wi-Fi stays on while streaming.

**Downloads.** A book goes into `/Audiobooks/<title>/`, together with `.abs.json` (item id,
server track order, per-track offsets, `complete`). An incomplete download is hidden from the
local list. The download runs only while its screen is open:
- each track is one streaming request from where the card's copy ends (`Range: bytes=N-`), so
  a download pays one TLS handshake per track; it reconnects from where it stopped when the
  connection drops (up to 5 times in a row without progress). Fetching it in 128 KB ranges, as
  streaming playback does to seek, cost a handshake per range and ran at ~50 KB/s through a
  Cloudflare tunnel;
- the network task (one long-lived task, stack in PSRAM) fills 64 KB PSRAM buffers;
- the main loop writes them to the card, because SD access from other tasks races the reader;
- leaving the screen pauses it, and Resume continues from the bytes already on the card.

**Progress sync.** Downloaded and streamed copies of a book share one local progress entry
(`abs:<itemId>` in `/.system/audio_progress.txt`). It is reconciled with `GET`/`PATCH
/api/me/progress/:id` without trusting either clock:
- if the server's `lastUpdate` changed since our last sync and we have not listened since, adopt
  the server position;
- if only we listened, push our position;
- if both changed, the further position wins.

Sync happens:
- **fetch:** on opening a book;
- **push:** on pause, track change, stop and finish, plus every 60 s while streaming.

For a downloaded book with Wi-Fi off, a push turns Wi-Fi on briefly (`AbsWifi`) and off again.

**Code:**
- `src/abs/`: `AbsClient` (API and tokens), `AbsWifi` (on-demand Wi-Fi with a user count),
  `AbsLocal` (downloaded-book metadata), `AbsDownloader`.
- `src/audio/HttpAudioFile.*`.
- `src/activity/settings/AbsActivity.*`.
- The player's feeder and sync tasks, in `AudioPlayer.cpp`.

## Next stages

- Chapters for M4B (Nero `chpl` or QuickTime chapter track), shown and skippable on the player.
- A player entry outside Device Management: Home widget, or a library action on audio files.
- Audiobookshelf chapters: the API returns them (`media.chapters`), but the player does not show them yet.
