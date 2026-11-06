# External USB-C audio module (LilyGo T5 S3)

An add-on for the `t5s3` firmware. A small module, an M5Stack ATOM Lite (ESP32-PICO-D4) plus an
optional PCM5102A I2S DAC, plugs into the T5's only USB-C port. One cable carries power and data:
the T5 powers the module from its charger's OTG boost and talks to the module's USB-to-UART
bridge as a USB host. Later stages play AudioBookShelf audiobooks through it, over Bluetooth A2DP
or wired.

Build the `t5s3_audio` env. The plain `t5s3` env does not contain any of this.

**The module firmware is not in this repository.** It lives in
[bigonionbots/atom-lite-audio-module](https://github.com/bigonionbots/atom-lite-audio-module),
which documents its build envs, PC test tools, DAC wiring, memory budget and decoders, and is the
home of the link protocol: `shared/audio_link/` here holds verbatim copies of its `protocol/`
(see the README there for how to update them). Module firmware 0.2 also takes the link on its
Grove port as a plain UART, for other hosts; the T5 uses the USB-C cable, which also powers it.

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
MCU-based FTDI emulation. Measured from a Linux PC (the module repository's `tools/`, kernel `ftdi_sio`) and from the T5
(Audio Module -> Link test):

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

1. Flash the ATOM from the module repository (`pio run -t upload` there; its `atom_linkdiag` env
   for the Link test below). Its LED turns blue. Optional: before unplugging it, note the bridge
   with `lsusb` (VID:PID).
2. Flash the T5: `pio run -e t5s3_audio -t upload`, then unplug the PC.
3. Plug the ATOM into the T5 with a C-to-C cable, then open Device Management -> Audio Module.
4. Tap **Module power**. Expected: VBUS shows `OTG boost` at about 5.1 V and the ATOM's LED
   lights up.
5. Tap **USB link**. Expected: BRIDGE shows `VID:PID driver product`.
6. Tap **Link test**. With a good bridge both rows show every byte with 0 bad and "exact".
   With this ATOM Lite's bridge, expect T5 -> MODULE clean, MODULE -> T5 paced exact, and
   unpaced damaged (see the bridge section).

From a PC instead: the module repository's `tools/link_diag.py` and `tools/echo_test.py`.

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
**Home -> Shortcuts -> Audiobooks** lists them under **On this device** (with covers), below
**Recently streamed**: the last Audiobookshelf books streamed (`/.system/audio_recent.json`, kept
up to date as they play), with their progress. Tapping one carries on: it opens on Audiobookshelf,
which asks for Wi-Fi if needed, and plays the downloaded copy if there is one, else streams it. Pick one and it resumes where you left off;
progress is in `/.system/audio_progress.txt`.
- The **Output** view picks Bluetooth headphones (Scan, tap the headset in pairing mode;
  it is remembered and reconnects), the wired DAC, or the silent test sink.
- Playback continues after closing the screen, so you can listen while reading. The T5 stays awake
  while a book plays. Pause, then sleep, and the module powers off.
- Confirm toggles play/pause and Up/Down change the volume.

**Chapters.** A book with chapters shows "Chapter 3 of 24: title" on Now Playing, with the time and
bar for the chapter, the position in the whole book under it, and |< / >| stepping by chapter
(|< restarts the chapter when it is more than 5 s in). **Chapters** lists them to jump to one.
They come from the server for Audiobookshelf books (`media.chapters`, also saved in a download's
`.abs.json`), or from the file for a single `.m4b`: a QuickTime chapter track, else Nero `chpl`
(`Mp4Source`; `test/gb_host/mp4_chapters_test.cpp` checks both on files ffmpeg makes). Books
without chapters (a folder of MP3s) step by file as before.

**Pieces:**

| Where | What |
| --- | --- |
| `shared/audio_link/AudioLinkProtocol.h` | Wire protocol, a verbatim copy from the module repository: framing with CRC-16, reliable go-back-N host frames, credit flow control on the ACK, STATUS/BT_DEVICE/LOG from the module. |
| `src/audio/` | `Mp3Source` (ID3 skip, Xing/VBRI duration and TOC seek) and `Mp4Source` (first AAC track to ADTS; stsz/stco read from the card on demand, so 20 h books are fine). The ADTS output is byte-identical to `ffmpeg -c:a copy -f adts`. |
| `src/system/AudioLink.*` | T5 end of the protocol: link task, resend window in PSRAM, status cache, Bluetooth scan results. |
| `src/system/AudioPlayer.*` | Books, tracks, progress. SD reads happen only on the main loop, into a 512 KB PSRAM queue, about a minute of audio. A sender task drains it to the link. This avoids racing the reader's own SD access, since SdFat is not thread-safe and most reader I/O does not take SdIoMutex. |
| `src/activity/settings/AudioPlayerActivity.*` | Now playing / Books / Output screens. |
| Module firmware | In its own repository (see the top of this page): it decodes MP3/AAC-LC (libhelix) and µ-law, resamples to 44.1 kHz stereo, and plays to Bluetooth A2DP, a PCM5102A DAC or a null sink; output and volume persist in its NVS. |

**Live streams (game sound, protocol 3).** Besides MP3 and AAC, a stream may be `Codec::MuLaw`:
G.711 µ-law bytes (`shared/audio_link/MuLaw.h`), with the sample rate and channel count in
STREAM_START (fields added in protocol 3; a protocol 2 host's shorter STREAM_START still works).
The module plays a live stream near real time: it starts once `LIVE_PRIME_MS` (60 ms) is waiting,
drops what arrives while more than `LIVE_MAX_BUFFERED_MS` (150 ms) is, and after running dry
waits for 60 ms again. STATUS `decodeErrors` counts those drops for a live stream. Games send 22050 Hz mono (see
`docs/games.md`); when the output runs at exactly twice the stream rate, as Bluetooth and I2S do
at 44.1 kHz, the module upsamples with `Resample2x.h`'s low-pass instead of its linear resampler. Both firmwares must be updated together for it.

**Module side.** Its build envs, PC stand-in (`tools/play.py`), DAC wiring, memory budget and
measurements are in the module repository's README. One limit shows on the T5: with no PSRAM the
module plays HE-AAC books as their AAC-LC core, which sounds duller.

**Licences:** the module firmware's binary is GPL-3.0 (it links libhelix); this firmware links
none of its libraries, and the protocol headers it copies depend on none of them.

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
Listen Again, a few books each with the listening progress. Under them:
- **A-Z**: every book by title (ignoring a leading The/A/An, as the server sorts), paged, with
  **Jump** to a letter;
- **Authors**: by surname, a book under each of its authors, then that author's books;
- **Search**: books whose title or author contains every word typed;
- **More**: switch library, **Refresh**, sign out.

**Podcasts.** A podcast library (`mediaType` "podcast") opens the same way, with its shelves
(Continue Listening and Newest Episodes are episodes, the others podcasts). A-Z, Authors and
Search list podcasts; opening one fetches it (`GET /api/items/:id?expanded=1`) and lists its
newest 100 episodes with date, length and the user's progress, newest first. An episode is
addressed as `<item id>/<episode id>` everywhere (shelves, the player, the recent list, the local
progress key `abs:<item>/<episode>`), which is also the tail of its progress routes (`GET`/`PATCH
/api/me/progress/:id/:episodeId`), so it streams and syncs like a book of one track. Its cover is
the podcast's. Episodes stream only: they cannot be downloaded yet.

**Offline first.** The libraries, the shelves and the whole library index are cached on the card
(`AbsLibrary`: `/.system/cache/abs/`), so the screen appears at once and A-Z, Authors and Search
work with Wi-Fi off; "Offline: showing the saved library" says so, and downloaded books open from
their `.abs.json`. With Wi-Fi up, entering the screen refreshes the shelves in the background
(the index too, at most every 10 minutes, or on Refresh), shown as "Updating..."; the screen
stays usable and swaps the new data in when it arrives. Only a first visit waits on "Loading".
The index (`AbsIndex`) keeps every string in one PSRAM pool, so a large library costs no internal
RAM; its page fetches parse JSON with a PSRAM allocator for the same reason.
`test/gb_host/abs_index_test.cpp` checks its order, authors, search and file format.

**Covers.** Shelf and list rows show each book's cover, its page a larger one. A cover is fetched
once, as a 240 px wide JPEG the server scales (`/api/items/:id/cover?width=240&format=jpeg`), and
kept in `/.system/cache/abs-covers/<item id>.jpg`, so it draws from the card at once and offline.
`AbsCovers` fetches missing ones on a long-lived task over one kept-alive connection (one TLS
handshake for a screenful), only while Wi-Fi is already up; the main loop writes them to the card,
and the screen redraws (at most every 2 s) when one it shows a placeholder for arrives. Until then
a dotted square stands in. Besides what is on screen, the whole library's missing covers are
prefetched in the background, four at a time, while the screen is open and Wi-Fi is up. A book
the server has no cover for (404) keeps the square until the next boot; any other failure (a
dropped connection is retried at once on a new one) is tried again after a minute. Delete the
folder to fetch them all again.

**Now Playing** shows the book's cover at the top left, with the title and chapter beside it. An
Audiobookshelf book uses the cached cover above (fetched when it opens, if online); a local book
uses `cover`/`folder`/`front` `.jpg`/`.png` in its folder, else its first image, else (a single
`.m4b`) the art embedded in the file (`covr`), copied once to `/.system/cache/audio-covers/`.

**Sign-in.** The server URL, username and password are entered once. The client uses the v2.26+
JWT scheme, checked against the v2.37.1 server source:
- `POST /login` with `x-return-tokens: true` returns `user.accessToken` and `user.refreshToken`.
- On a 401, `POST /auth/refresh` with `x-refresh-token` returns a new access token and a rotated
  refresh token, and the request is retried once.
- Older servers fall back to the legacy `user.token`.
- ESP-IDF 5's `esp_http_client_perform()` returns `ESP_ERR_NOT_SUPPORTED`, not `ESP_OK`, for a
  401 whose `WWW-Authenticate` is not Basic/Digest, which is every 401 from Audiobookshelf. The
  client treats that as the 401 reply it is; otherwise an expired access token (about an hour)
  showed as "Network error: ESP_ERR_NOT_SUPPORTED" and was never refreshed.

The password is never stored. The tokens are saved in `/.system/audiobookshelf.json`. A refresh
token unused for 30 days (the server default) means signing in again.

**Streaming.** `HttpAudioFile` reads a track with HTTP Range requests (`GET
/api/items/:id/file/:ino`, served by Express `sendFile`). It keeps three 128 KB blocks in PSRAM:
the audio being played, plus the MP4 sample tables elsewhere in the file. Underneath, the same
`Mp3Source`/`Mp4Source` readers run on a feeder task, so a streamed book needs no SD access.
Network errors are retried. If retries fail, playback restarts from the current position after
5 s. Wi-Fi stays on while streaming, but a streamed book paused for 3 minutes stops its stream
and lets Wi-Fi go ("Wi-Fi off while paused"); Play reconnects with the saved networks and
restarts it at the same place ("Reconnecting Wi-Fi..." until it plays).

**Downloads.** A book goes into `/Audiobooks/<title>/`, together with `.abs.json` (item id,
server track order, per-track offsets, chapters, `complete`). An incomplete download is hidden
from the local list.

**The download queue** (`AbsQueue`, saved in `/.system/abs_queue.json`). **Download to device**
adds a book to the queue; it downloads in the background, whatever screen is open, while the T5
is on a charger (the BQ25896 sees external power on VBUS; the boost that powers the audio module
does not count). On the charger the T5 stays awake until the queue is done, then sleeps as usual;
unplugging pauses a charger-only download, which resumes from the card next time. The
**Downloads** page (Audiobooks page, Audiobookshelf -> More, or a queued book's page) lists the
queue with each book's state and progress; select a book to move it **Up** / **Down**, start it
**Now** (also on battery: it then keeps the T5 awake only while the page is open), or **Remove**
it (which deletes what it had downloaded). A book that fails (no Wi-Fi, server down) is tried
again after 10 minutes and does not keep the T5 awake meanwhile. The queue fetches each book's
details on a task of its own, holding a Wi-Fi share (saved networks) until its download ends.

How a book downloads:
- each track is one streaming request from where the card's copy ends (`Range: bytes=N-`), so
  a download pays one TLS handshake per track; it reconnects from where it stopped when the
  connection drops (up to 5 times in a row without progress). Fetching it in 128 KB ranges, as
  streaming playback does to seek, cost a handshake per range and ran at ~50 KB/s through a
  Cloudflare tunnel;
- the network task (one long-lived task, stack in PSRAM) fills 64 KB PSRAM buffers;
- the main loop writes them to the card (`AbsQueue::service()` -> `AbsDownloader::service()`),
  because SD access from other tasks races the reader;
- a paused download continues from the bytes already on the card.

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

- A player entry outside Device Management: Home widget, or a library action on audio files.
