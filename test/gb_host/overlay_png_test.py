"""Checks OverlayPng::decode() through overlay_png_dump. Usage: overlay_png_test.py <dump> <template.png> <dir>"""

import subprocess
import sys
from pathlib import Path

from PIL import Image

dump, template, out = sys.argv[1], Path(sys.argv[2]), Path(sys.argv[3])
W, H = 960, 540


def run(png):
    ink, opaque = out / (png.stem + ".ink.pbm"), out / (png.stem + ".opaque.pbm")
    result = subprocess.run([dump, str(png), str(ink), str(opaque)], capture_output=True, text=True)
    if result.returncode != 0:
        sys.exit(f"{png.name}: decode failed: {result.stdout.strip()}")
    # PBM bit 1 shows black, which Pillow reads back as 0. Return plain predicates instead:
    # black(x, y) for the ink plane, drawn(x, y) for the opaque plane.
    ink_px = Image.open(ink).convert("L").load()
    opaque_px = Image.open(opaque).convert("L").load()
    return (lambda x, y: ink_px[x, y] == 0), (lambda x, y: opaque_px[x, y] == 0)


def check(name, ok):
    print(f"overlay {name}: {'ok' if ok else 'FAIL'}")
    if not ok:
        sys.exit(1)


# The template's guides are 40% opaque: nothing may be drawn.
_, drawn = run(template)
check("template draws nothing", not any(drawn(x, y) for y in range(H) for x in range(W)))

# A test overlay: opaque black block, opaque white block, 50% grey block, 40%-alpha block.
base = Image.new("RGBA", (W, H), (0, 0, 0, 0))
for x0, colour in ((0, (0, 0, 0, 255)), (100, (255, 255, 255, 255)), (200, (128, 128, 128, 255)), (300, (0, 0, 0, 100))):
    base.paste(colour, (x0, 0, x0 + 80, 80))

variants = {
    "rgba": base,
    "la": base.convert("LA"),
    "palette": base.quantize(colors=8, method=Image.Quantize.FASTOCTREE),
}
for kind, img in variants.items():
    path = out / f"overlay-{kind}.png"
    img.save(path)
    black, drawn = run(path)
    opaque_ok = drawn(40, 40) and drawn(140, 40) and drawn(240, 40) and not drawn(340, 40) and not drawn(500, 300)
    black_ok = black(40, 40) and not black(140, 40)
    grey = sum(1 for y in range(80) for x in range(200, 280) if black(x, y))
    grey_ok = 0.35 < grey / 6400 < 0.65
    check(f"{kind} planes", opaque_ok and black_ok and grey_ok)

# Wrong size is refused.
small = out / "overlay-small.png"
Image.new("RGBA", (100, 100)).save(small)
r = subprocess.run([dump, str(small), str(out / "x.pbm"), str(out / "y.pbm")], capture_output=True, text=True)
check("wrong size refused", r.returncode == 3 and "960 x 540" in r.stdout)
