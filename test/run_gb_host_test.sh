#!/usr/bin/env bash
# Host check of the vendored Game Boy core (third_party/crankboy) against the MIT test ROMs
# dmg-acid2 and cgb-acid2. Downloads them into build/gb_host/ on first run, then writes each
# ROM's frame after 120 frames as build/gb_host/<rom>.pgm. dmg-acid2 must match its reference
# exactly; cgb-acid2 is converted to grey by the core, so check build/gb_host/cgb-acid2.pgm by eye.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$ROOT_DIR/build/gb_host"
BINARY="$BUILD_DIR/gb_frame_dump"

mkdir -p "$BUILD_DIR"

fetch() {
  [ -f "$BUILD_DIR/$2" ] || curl -sSfL -o "$BUILD_DIR/$2" "$1"
}
fetch https://github.com/mattcurrie/dmg-acid2/releases/download/v1.0/dmg-acid2.gb dmg-acid2.gb
fetch https://github.com/mattcurrie/cgb-acid2/releases/download/v1.1/cgb-acid2.gbc cgb-acid2.gbc
fetch https://raw.githubusercontent.com/mattcurrie/dmg-acid2/master/img/reference-dmg.png dmg-acid2-reference.png

cc -std=gnu11 -O2 -w -Werror=implicit-function-declaration -DGB_HOST_TEST \
  "$ROOT_DIR/test/gb_host/gb_frame_dump.c" \
  "$ROOT_DIR/src/games/gb/GbApu.c" \
  "$ROOT_DIR/src/games/gb/GbPort.c" \
  "$ROOT_DIR/src/games/gb/GbSession.c" \
  -o "$BINARY"

# Sound: uCity's music (GPL-3.0 game, downloaded for this test only, never shipped) must come out
# as music; the core's half-rate mode turned it into noise. See test/gb_host/gb_audio_test.c.
fetch https://github.com/AntonioND/ucity/releases/download/v1.3/ucity_compat.gbc ucity_compat.gbc
cc -std=gnu11 -O2 -w -Werror=implicit-function-declaration -DGB_HOST_TEST \
  "$ROOT_DIR/test/gb_host/gb_audio_test.c" \
  "$ROOT_DIR/src/games/gb/GbApu.c" \
  "$ROOT_DIR/src/games/gb/GbPort.c" \
  "$ROOT_DIR/src/games/gb/GbSession.c" \
  -lm -o "$BUILD_DIR/gb_audio_test"
"$BUILD_DIR/gb_audio_test" "$BUILD_DIR/ucity_compat.gbc" 3600 "$BUILD_DIR/ucity.wav"
# Held notes, low and high: each must come out at its pitch (no glitches at render chunk joins).
for hz in 65 880; do
  python3 "$ROOT_DIR/test/gb_host/make_tone_rom.py" "$BUILD_DIR/tone$hz.gb" "$hz" >/dev/null
done
"$BUILD_DIR/gb_audio_test" "$BUILD_DIR/tone65.gb" 300 - 65.0
"$BUILD_DIR/gb_audio_test" "$BUILD_DIR/tone880.gb" 300 - 879.7

"$BINARY" "$BUILD_DIR/dmg-acid2.gb" 120 "$BUILD_DIR/dmg-acid2.pgm"
"$BINARY" "$BUILD_DIR/cgb-acid2.gbc" 120 "$BUILD_DIR/cgb-acid2.pgm"

python3 - "$BUILD_DIR" <<'PY'
import sys
from PIL import Image
d = sys.argv[1]
ours = Image.open(f"{d}/dmg-acid2.pgm").convert("L")
ref = Image.open(f"{d}/dmg-acid2-reference.png").convert("L")
diff = sum(1 for p, q in zip(ours.tobytes(), ref.tobytes()) if p != q)
print(f"dmg-acid2: {diff} pixels differ from the reference")
sys.exit(1 if diff else 0)
PY

c++ -std=c++20 -O2 -Wall -Wextra "$ROOT_DIR/test/gb_host/video_row_test.cpp" -o "$BUILD_DIR/video_row_test"
"$BUILD_DIR/video_row_test"

# Overlay PNG decoder: the shipped template must decode to "nothing drawn", and test images of
# each colour type must decode to the expected planes.
# lib/uzlib ships only the inflater; its unused checksum helper is dropped with --gc-sections.
cc -O2 -ffunction-sections -c "$ROOT_DIR/lib/uzlib/src/tinflate.c" -o "$BUILD_DIR/tinflate.o"
c++ -std=c++20 -O2 -Wall -Wextra -DGB_HOST_TEST -I"$ROOT_DIR/lib/InflateReader" -I"$ROOT_DIR/lib/uzlib/src" \
  "$ROOT_DIR/test/gb_host/overlay_png_dump.cpp" "$ROOT_DIR/src/games/OverlayPng.cpp" \
  "$ROOT_DIR/lib/InflateReader/InflateReader.cpp" "$BUILD_DIR/tinflate.o" -Wl,--gc-sections \
  -o "$BUILD_DIR/overlay_png_dump"
python3 "$ROOT_DIR/test/gb_host/overlay_png_test.py" "$BUILD_DIR/overlay_png_dump" "$ROOT_DIR/docs/games/overlay-template.png" "$BUILD_DIR"

# Game sound's link path (shared/audio_link/MuLaw.h, Resample2x.h), also on the uCity music above.
c++ -std=c++20 -O2 -Wall -Wextra -I"$ROOT_DIR/shared/audio_link" "$ROOT_DIR/test/gb_host/live_audio_test.cpp" \
  -o "$BUILD_DIR/live_audio_test"
"$BUILD_DIR/live_audio_test" "$BUILD_DIR/ucity.wav"
