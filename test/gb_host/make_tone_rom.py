#!/usr/bin/env python3
"""Writes a minimal Game Boy ROM that holds one square-wave note on channel 2, for sound tests.

Usage: make_tone_rom.py OUT.gb HZ

The ROM switches sound on (full volume, both sides), sets channel 2 to a 50% duty square at the
nearest frequency the hardware has (131072 / (2048 - x) Hz), triggers it and loops forever. A
held note makes per-frame glitches obvious: test/gb_host/gb_audio_test.c checks its pitch.
"""

import sys

LOGO = bytes.fromhex(
    "CEED6666CC0D000B03730083000C000D0008111F8889000EDCCC6EE6DDDDD999BBBB67636E0EECCCDDDC999FBBB9333E"
)


def main() -> None:
    out, hz = sys.argv[1], float(sys.argv[2])
    x = max(0, min(2047, round(2048 - 131072 / hz)))
    rom = bytearray(32768)
    rom[0x100:0x104] = bytes([0x00, 0xC3, 0x50, 0x01])  # nop; jp $0150
    rom[0x104:0x134] = LOGO
    rom[0x134:0x13F] = b"TONETEST".ljust(11, b"\0")
    code = []

    def store(register: int, value: int) -> None:
        code.extend([0x3E, value, 0xE0, register])  # ld a, value; ldh (register), a

    store(0x26, 0x80)  # NR52: sound on
    store(0x24, 0x77)  # NR50: full volume
    store(0x25, 0xFF)  # NR51: every channel on both sides
    store(0x16, 0x80)  # NR21: 50% duty
    store(0x17, 0xF0)  # NR22: volume 15, no envelope
    store(0x18, x & 0xFF)  # NR23: frequency low
    store(0x19, 0x80 | (x >> 8))  # NR24: trigger, frequency high
    code.extend([0x18, 0xFE])  # jr -2
    rom[0x150 : 0x150 + len(code)] = bytes(code)
    check = 0
    for i in range(0x134, 0x14D):
        check = (check - rom[i] - 1) & 0xFF
    rom[0x14D] = check
    total = sum(rom) - rom[0x14E] - rom[0x14F]
    rom[0x14E], rom[0x14F] = (total >> 8) & 0xFF, total & 0xFF
    with open(out, "wb") as f:
        f.write(rom)
    print(f"{out}: {131072 / (2048 - x):.1f} Hz (x = {x})")


if __name__ == "__main__":
    main()
