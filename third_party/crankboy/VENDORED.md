# CrankBoy Game Boy core, vendored

Source: https://github.com/CrankBoyHQ/crankboy-app at commit `31cb280` (2026-11-01), MIT
(see LICENSE). Peanut-GB (MIT, Mahyar Koshkouei, with MIT-licensed SameBoy parts) and MiniGB
APU (MIT, see libs/minigb_apu/LICENSE) come with it.

Kept, unmodified:
- `libs/peanut_gb.h`, `libs/peanut_gb_core.h`, `libs/pgb/` (the emulator core, DMG and CGB)
- `libs/minigb_apu/` (sound)

Not kept: the Playdate app itself (`src/`, UI, scenes, file handling) and the other libraries.

## Shim

The core includes a handful of the Playdate app's headers by relative path (`../src/app.h`,
`utility.h`, `preferences.h`, `dtcm.h`, `scenes/game_scene.h`). Those names in `src/` here are
inx-pro's own one-line files that include `src/crankboy_shim.h`, which supplies only what the
core touches: attribute macros, the preference globals, a minimal `playdate` API object, the
allocator and the few game-scene fields the audio callback reads. The definitions are in
`src/games/gb/GbPort.c`.

The shim builds the core as CrankBoy's desktop simulator does (`TARGET_SIMULATOR`). The device
build puts hot code in custom sections that CrankBoy relocates into tightly coupled memory on
the Playdate; the ESP-IDF linker has no rule for them.

## Updating

Copy the files above from a newer commit, then run `test/run_gb_host_test.sh` and build the
`t5s3_audio` env. A changed core may need new names in the shim.
