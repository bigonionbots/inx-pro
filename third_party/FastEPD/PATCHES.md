# FastEPD, vendored

Source: https://github.com/bitbank2/FastEPD at commit `60d4945` (2026-10-20), Apache-2.0
(see LICENSE, Copyright 2024 BitBank Software, Inc.). Only `src/`, the licence and
library.properties are kept.

Used by the `t5s3_audio` env's game mode (`src/games/`) to drive the LilyGo T5 S3 Pro Lite
(H752-01) panel directly while a game runs. inx-pro's normal UI keeps using freeink-sdk.

## Changes

- `FASTEPD::initCustomPanel()` (src/FastEPD.cpp) now does what `bbepInitPanel()` does for a
  predefined panel: it fills in the panel state, runs `bbepIOInit()` (which calls the custom
  IO-init callback and then sets up the ESP32-S3 parallel bus) and allocates the frame buffers
  with `bbepSetPanelSize()`. Upstream only called the IO-init callback, so a custom panel had no
  bus and no buffers. A custom panel that passes no gray matrix gets FastEPD's ED047TC1-class
  `u8M5Matrix`, since the built-in matrices are not reachable from outside the file. The H752-01 board support itself lives in inx-pro (`src/games/GameDisplay.cpp`),
  built on freeink-sdk's power hooks.
