#!/usr/bin/env python3
"""Generate the game-screen overlay template and an example overlay (960 x 540 PNGs).

The layout comes from src/games/GameUi.h, so both always match the firmware. Outputs:
  docs/games/overlay-template.png               the template, for viewing and editing
  docs/games/overlay-example.png                a finished overlay drawn from the same layout
  src/games/generated/OverlayTemplatePng.h      both PNGs as byte arrays; the firmware writes them
                                                to /Games/Overlays/Template.png and Example.png
                                                when they are missing

Everything in the template is drawn at 40% opacity. The firmware only draws pixels that are at
least 50% opaque, so the guides never show on the device: paint opaque artwork over them.

Needs Pillow. Run from anywhere: python3 scripts/make_game_overlay_template.py
"""

import io
import re
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parent.parent
LAYOUT = ROOT / "src/games/GameUi.h"
TEMPLATE_OUT = ROOT / "docs/games/overlay-template.png"
EXAMPLE_OUT = ROOT / "docs/games/overlay-example.png"
HEADER_OUT = ROOT / "src/games/generated/OverlayTemplatePng.h"

WIDTH, HEIGHT = 960, 540
GUIDE_ALPHA = 102  # 40%: below the firmware's 50% threshold


def layout_constants():
    """constexpr ints from GameUi.h, evaluated in order (later ones may use earlier ones)."""
    values = {}
    for name, expr in re.findall(r"constexpr int (\w+) = ([^;]+);", LAYOUT.read_text()):
        values[name] = int(eval(expr, {}, dict(values)))  # noqa: S307 - our own header
    return values


def png_bytes(image):
    buffer = io.BytesIO()
    image.save(buffer, "PNG", optimize=True)
    return buffer.getvalue()


def c_array(name, data):
    rows = [", ".join(f"0x{b:02x}" for b in data[i:i + 16]) for i in range(0, len(data), 16)]
    return (f"constexpr size_t {name}_SIZE = {len(data)};\n"
            f"alignas(4) static const uint8_t {name}[] = {{\n    " + ",\n    ".join(rows) + "};\n")


def template(c):
    """Guides for every control and the game screen, all 40% opaque."""
    image = Image.new("RGBA", (WIDTH, HEIGHT), (0, 0, 0, 0))
    draw = ImageDraw.Draw(image)
    font = ImageFont.load_default(size=15)
    small = ImageFont.load_default(size=12)
    red = (220, 0, 0, GUIDE_ALPHA)
    blue = (0, 60, 220, GUIDE_ALPHA)
    grey = (128, 128, 128, GUIDE_ALPHA)
    white = (255, 255, 255, GUIDE_ALPHA)

    def label(xy, text, fill=red, anchor="mm", f=font):
        draw.text(xy, text, fill=fill, font=f, anchor=anchor)

    # Top bar.
    bar = c["BAR_H"]
    draw.rectangle([0, 0, WIDTH - 1, bar - 1], outline=blue)
    label((8, 14), "game title", fill=blue, anchor="lm", f=small)
    label((8, 38), "status line (frame rate) - redrawn by the firmware", fill=blue, anchor="lm", f=small)
    for name, x, w in (("LIGHT", "LIGHT_X", "LIGHT_W"), ("-", "DIM_X", "STEP_W"), ("+", "BRIGHT_X", "STEP_W"),
                       ("EXIT", "EXIT_X", "EXIT_W")):
        draw.rectangle([c[x], 0, c[x] + c[w] - 1, bar - 1], outline=red, width=2)
        label((c[x] + c[w] // 2, bar // 2), name)

    # D-pad.
    dx, dy, arm, half = c["DPAD_X"], c["DPAD_Y"], c["DPAD_ARM"], c["DPAD_WIDTH"] // 2
    draw.rectangle([dx - half, dy - arm, dx + half, dy + arm], outline=red, width=2)
    draw.rectangle([dx - arm, dy - half, dx + arm, dy + half], outline=red, width=2)
    dead = c["DPAD_DEAD"]
    draw.ellipse([dx - dead, dy - dead, dx + dead, dy + dead], outline=red)
    label((dx, dy - arm - 14), "D-PAD (8-way)")
    label((dx, dy + arm + 14), "the whole left column is", f=small)
    label((dx, dy + arm + 30), "touch area, by direction", f=small)

    # A and B: the drawn button and the area that counts as a press.
    for name, x, y in (("A", c["A_X"], c["A_Y"]), ("B", c["B_X"], c["B_Y"])):
        r = c["AB_R"]
        draw.ellipse([x - r, y - r, x + r, y + r], outline=red, width=2)
        reach = c["AB_REACH"]
        draw.ellipse([x - reach, y - reach, x + reach, y + reach], outline=(220, 0, 0, GUIDE_ALPHA // 2))
        label((x, y), name)
    label(((c["A_X"] + c["B_X"]) // 2, (c["A_Y"] + c["B_Y"]) // 2 + 4), "A+B", f=small)
    label((c["AREA_X"] + c["AREA_W"] + 120, c["AREA_Y"] + 60), "outer rings: touch reach", f=small)

    # SELECT / START.
    top = c["BOTTOM_Y"] + 6
    bottom = HEIGHT - 4
    for name, x in (("SELECT", "SELECT_X"), ("START", "START_X")):
        draw.rectangle([c[x], top, c[x] + c["SMALL_W"] - 1, bottom - 1], outline=red, width=2)
        label((c[x] + c["SMALL_W"] // 2, (top + bottom) // 2), name)

    # Sound: the readout (redrawn by the firmware, like LIGHT) and the volume steps.
    for name, x, w in (("SOUND", "SOUND_X", "SOUND_W"), ("-", "QUIETER_X", "SOUND_STEP_W"),
                       ("+", "LOUDER_X", "SOUND_STEP_W")):
        draw.rectangle([c[x], top, c[x] + c[w] - 1, bottom - 1], outline=red, width=2)
        label((c[x] + c[w] // 2, (top + bottom) // 2 - (8 if name == "SOUND" else 0)), name)
    label((c["SOUND_X"] + c["SOUND_W"] // 2, (top + bottom) // 2 + 10), "volume", f=small)

    # Game screen, drawn last so it covers the touch rings: the overlay is never drawn there.
    ax, ay, aw, ah = c["AREA_X"], c["AREA_Y"], c["AREA_W"], c["AREA_H"]
    draw.rectangle([ax, ay, ax + aw - 1, ay + ah - 1], fill=grey)
    label((ax + aw // 2, ay + ah // 2 - 12), "GAME SCREEN", fill=white)
    label((ax + aw // 2, ay + ah // 2 + 12), "always shows the game; the overlay is not drawn here", fill=white,
          f=small)

    # How to use it, in the bottom-left corner.
    notes = [
        "inx-pro game overlay template, 960 x 540",
        "Paint OPAQUE artwork; transparent = default screen.",
        "Guides are 40% opaque and never show.",
        "Save as PNG in /Games/Overlays/.",
    ]
    for i, line in enumerate(notes):
        label((6, c["BOTTOM_Y"] + 4 + 13 * i), line, fill=blue, anchor="lt", f=small)
    return image


def example(c):
    """A finished overlay: grey body, solid controls. Every pixel is fully opaque or fully clear."""
    image = Image.new("RGBA", (WIDTH, HEIGHT), (0, 0, 0, 0))
    draw = ImageDraw.Draw(image)
    big = ImageFont.load_default(size=22)
    small = ImageFont.load_default(size=16)
    grey, black, white = (150, 150, 150, 255), (0, 0, 0, 255), (255, 255, 255, 255)
    draw.rectangle([0, c["BAR_H"], WIDTH - 1, HEIGHT - 1], fill=grey)
    draw.rectangle([c["AREA_X"] - 8, c["AREA_Y"], c["AREA_X"] + c["AREA_W"] + 7, c["AREA_Y"] + c["AREA_H"] + 5],
                   fill=black)
    dx, dy, arm, half = c["DPAD_X"], c["DPAD_Y"], c["DPAD_ARM"], c["DPAD_WIDTH"] // 2
    draw.rectangle([dx - half, dy - arm, dx + half, dy + arm], fill=black)
    draw.rectangle([dx - arm, dy - half, dx + arm, dy + half], fill=black)
    for name, x, y in (("A", c["A_X"], c["A_Y"]), ("B", c["B_X"], c["B_Y"])):
        r = c["AB_R"]
        draw.ellipse([x - r, y - r, x + r, y + r], fill=black)
        draw.text((x, y), name, fill=white, font=big, anchor="mm")
    top = c["BOTTOM_Y"] + 12
    for name, x in (("SELECT", "SELECT_X"), ("START", "START_X")):
        draw.rounded_rectangle([c[x], top, c[x] + c["SMALL_W"], HEIGHT - 8], radius=14, fill=black)
        draw.text((c[x] + c["SMALL_W"] // 2, (top + HEIGHT - 8) // 2), name, fill=white, font=small, anchor="mm")
    # Sound: the firmware draws the SOUND readout (the volume), so leave a clear window over it, as
    # the top bar is left clear for LIGHT; - and + are artwork.
    sound_top = c["BOTTOM_Y"] + 6
    draw.rectangle([c["SOUND_X"], sound_top, c["SOUND_X"] + c["SOUND_W"] - 1, HEIGHT - 5], fill=(0, 0, 0, 0))
    for name, x in (("-", "QUIETER_X"), ("+", "LOUDER_X")):
        draw.rounded_rectangle([c[x], top, c[x] + c["SOUND_STEP_W"] - 1, HEIGHT - 8], radius=14, fill=black)
        draw.text((c[x] + c["SOUND_STEP_W"] // 2, (top + HEIGHT - 8) // 2), name, fill=white, font=big, anchor="mm")
    draw.text((c["DPAD_X"], c["BOTTOM_Y"] + 20), "inx-pro", fill=black, font=big, anchor="mm")
    return image


def main():
    c = layout_constants()
    guide = png_bytes(template(c))
    sample = png_bytes(example(c))
    TEMPLATE_OUT.parent.mkdir(parents=True, exist_ok=True)
    TEMPLATE_OUT.write_bytes(guide)
    EXAMPLE_OUT.write_bytes(sample)

    HEADER_OUT.parent.mkdir(parents=True, exist_ok=True)
    HEADER_OUT.write_text(
        "#pragma once\n\n"
        "/**\n"
        " * @file OverlayTemplatePng.h\n"
        " * @brief Game overlay template and example PNGs. Generated by scripts/make_game_overlay_template.py: "
        "do not edit.\n"
        " */\n\n"
        "#include <cstddef>\n#include <cstdint>\n\n"
        "// clang-format off\n"
        + c_array("OVERLAY_TEMPLATE_PNG", guide) + "\n" + c_array("OVERLAY_EXAMPLE_PNG", sample)
        + "// clang-format on\n"
    )
    print(f"template {len(guide)} bytes, example {len(sample)} bytes; {HEADER_OUT.relative_to(ROOT)} written")


if __name__ == "__main__":
    main()
