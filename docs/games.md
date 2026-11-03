# Games (LilyGo T5 S3 Pro Lite, `t5s3_audio` env)

Game Boy / Game Boy Color and NES games on the T5, from ROMs in `/Games` on the SD card. No games
are included.

## Licensing: why these components

inx-pro is MIT, and the games feature keeps it that way: every component is MIT or Apache-2.0.

| Part | Component | License | Notes |
| --- | --- | --- | --- |
| GB / GBC core | CrankBoy's Peanut-GB (`libs/peanut_gb.h`, `peanut_gb_core.h`, `pgb/`) | MIT | Supports GBC; SameBoy-derived parts are MIT too. It needs a small shim for its Playdate app headers. |
| GB sound | MiniGB APU (from CrankBoy) | MIT | |
| NES core | [agnes](https://github.com/kgabis/agnes) | MIT | Mappers 0, 1, 2, 4 (NROM, MMC1, UxROM, MMC3). 3 and 7 to be added. No APU. |
| Panel driver | [FastEPD](https://github.com/bitbank2/FastEPD) | Apache-2.0 | Vendored in `third_party/FastEPD` with one patch (PATCHES.md). |
| H752-01 board support | `src/games/GameDisplay.cpp` | MIT | Built on freeink-sdk's own power hooks. |

Not used, and why:
- **InkDeck / Book32** integration code is AGPL-3.0, which would turn the firmware into AGPL.
  Its FastEPD video additions (`InkDeck*Video.h`) and its LilyGo display glue are part of that
  code.
- **Nofrendo** in every copy checked has per-file LGPL headers but ships with a GPL-2.0 `COPYING`:
  - retro-go also adds header-less files under its GPL-2.0;
  - Espressif's esp32-nesemu README calls it GPLv2.

  Either way it is not clean for an MIT firmware.

## Architecture

The SDK owns the panel through a private LovyanGFX device. It refreshes the whole panel per push
and cannot hand the bus over and take it back. So games run in a **separate boot** (`src/games/GameMode.*`):

1. Launching writes a record (what to run) to RTC memory and restarts.
2. `setup()` brings up GPIO (touch, PCA9535) and SD, sees the record **before** the SDK display
   starts, and calls `GameMode::run()`. The record is cleared first, so a crash returns to the reader.
3. `GameDisplay` drives the panel with FastEPD as a custom panel:
   - FastEPD's parallel bus on the SDK's data and timing pins;
   - gate-driver row control on GPIOs (STV 45, CKV 48, LE 42);
   - power and output enable through the SDK's `lilygoT5S3LgfxConfig().power` hooks (PCA9535 + TPS65185).
4. Leaving restarts into the normal UI.

Touch uses the GT911 multi-touch snapshot (`HalGPIO::touchSnapshot()`), so a direction and a
button can be held together.

Game mode never reaches BootActivity, so `GameLight` applies the saved frontlight setting on entry.
Changes made in game are written back when leaving, so the reader comes back at the same level.

## Milestones

1. **Panel and touch test** (done, needs a hardware check): Home -> Shortcuts -> Games -> Display
   and touch test.
2. **Game Boy / Color**:
   - the core with its shim, verified on the host against the MIT test ROMs dmg-acid2 and
     cgb-acid2;
   - then on the device: ROM library, 3x scaled dithered video, touch controls and battery saves.
3. **NES** with agnes (+ mappers 3 and 7).
4. **Speed and sound**:
   - frame pacing and partial-update tuning;
   - game audio through the external audio module (needs a raw PCM path on the ATOM).

## Milestone 1 test

The test screen:
- shows the four Game Boy shades as dither patches;
- bounces a block inside a 480 x 432 frame (a Game Boy screen at 3x);
- draws a circle under every finger.

Every 8 s the bottom line reports the partial-update time, alternating between updating only the
rows the block moves through and updating the whole game area. The top bar has LIGHT (on/off,
with the level), `-` and `+` for the frontlight. EXIT (top right) or the power button returns.
