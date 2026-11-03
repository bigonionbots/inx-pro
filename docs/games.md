# Games (LilyGo T5 S3 Pro Lite, `t5s3_audio` env)

Game Boy / Game Boy Color and NES games on the T5, from ROMs in `/Games` on the SD card. No games
are included.

## Licensing: why these components

inx-pro is MIT, and the games feature keeps it that way: every component is MIT or Apache-2.0.

| Part | Component | License | Notes |
| --- | --- | --- | --- |
| GB / GBC core | CrankBoy's Peanut-GB, vendored in `third_party/crankboy` (VENDORED.md) | MIT | Supports GBC; SameBoy-derived parts are MIT too. inx-pro's shim stands in for its Playdate app headers. |
| GB sound | MiniGB APU (from CrankBoy) | MIT | Built, not played yet (milestone 4). |
| NES core | [agnes](https://github.com/kgabis/agnes) | MIT | Mappers 0, 1, 2, 4 (NROM, MMC1, UxROM, MMC3). 3 and 7 to be added. No APU. |
| Panel driver | [FastEPD](https://github.com/bitbank2/FastEPD) | Apache-2.0 | Vendored in `third_party/FastEPD` with two patches (PATCHES.md). |
| H752-01 board support | `src/games/GameDisplay.cpp` | MIT | Built on freeink-sdk's own power hooks. |

Not used, and why:
- **T5S3-GameBoy** (ShallowGreen123) runs the same CrankBoy core on this board family, but the
  repository has no licence, so none of its code is used. Its display approach (a continuous
  scan with per-pixel drive counters, which FastEPD's Linux `videoUpdate()` also uses) is
  re-implemented independently in `GameVideo`.
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

### Video

FastEPD's partial update runs four full panel scans and a clearing scan for every update, so
the milestone 1 test managed about 8 fps. Games use `GameVideo` instead:
- a task on core 0 scans the panel continuously;
- each scan compares the frame buffer with what each pixel is being driven to; a changed pixel
  is driven for 4 scans towards its new colour, then left alone (`GameVideoRow.h`, 3-bit
  counters in bit planes);
- rows with nothing changed or settling get an all-zero row, which is cheap.

Drawing and refreshing overlap: the game draws a frame whenever it likes, marks the rows it
changed, and the next scan picks them up.

### Game Boy

`GameBoy.cpp` loads the ROM (up to 4 MB) into PSRAM and runs the core through a small C
interface (`gb/GbSession.h`). Colour games run in Color mode, and the core turns their colours
into four greys. Each frame is drawn 3x (480 x 432) with a 2x2 dither, and only the rows that
changed are marked. Controls: an 8-way D-pad on the left, A and B on the right (touching
between them presses both), SELECT and START under the game. Battery saves go next to the ROM
as `<name>.sav` (the usual raw format), written 3 s after the game stops writing and on exit.

### Overlays

An overlay is a 960 x 540 PNG in `/Games/Overlays` that changes how the game screen looks. It
never changes what the screen does: the touch areas stay put. Choose one with the Overlay button
at the top of the Games screen (stored in `/Games/.overlay`).

- Pixels at least 50% opaque are drawn: dark ones black, light ones white, greys as a 4x4
  dither. More transparent pixels leave the normal screen showing.
- The game screen (480 x 432 at 240, 54) always shows the game; the overlay is not drawn there.
- The LIGHT readout and the status line are redrawn while playing, and the overlay is drawn
  over them again each time, so covering them hides them.

**Template.** The Games screen writes `/Games/Overlays/Template.png` when it is missing (also in
the repository as `docs/games/overlay-template.png`). It marks the game screen, the top-bar
buttons, the D-pad, A and B with their touch reach, and SELECT / START. Everything in it is 40%
opaque, so the template itself draws nothing: paint opaque artwork over the guides, save as PNG
under a new name, and pick it. Because the template draws nothing, choosing it looks the same as
None. `Example.png` (`docs/games/overlay-example.png`) is written next to it: a finished overlay
on the same layout, to see one working. `scripts/make_game_overlay_template.py` regenerates both
PNGs and the firmware's copy (`src/games/generated/OverlayTemplatePng.h`) from the positions in
`src/games/GameUi.h`; rerun it after moving a control.

`OverlayPng.cpp` decodes the PNG (any colour type, 8 or 16 bits, palettes, tRNS; not interlaced)
into two 1-bit planes when the game starts.

### Host tests

`test/run_gb_host_test.sh` builds the core on the PC, runs dmg-acid2 and cgb-acid2 (MIT test
ROMs, downloaded on first run) and checks dmg-acid2 against its reference image. It also tests
the per-pixel drive logic and the overlay PNG decoder (the template must draw nothing; RGBA,
grey + alpha and palette test images must decode as expected).

Game mode never reaches BootActivity, so `GameLight` applies the saved frontlight setting on entry.
Changes made in game are written back when leaving, so the reader comes back at the same level.

## Milestones

1. **Panel and touch test** (done; the test screen has since been removed).
2. **Game Boy / Color** (done, checked on the device with Game Boy games; Color untested):
   - the core with its shim, verified on the host against dmg-acid2 and cgb-acid2 (done);
   - on the device: ROM list, 3x dithered video on the continuous scan, touch controls and
     battery saves.
3. **NES** with agnes (+ mappers 3 and 7).
4. **Speed and sound**:
   - frame pacing and partial-update tuning;
   - game audio through the external audio module (needs a raw PCM path on the ATOM).
