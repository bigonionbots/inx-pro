# Audio module link protocol (verbatim copies)

The T5 talks to the external audio module (an M5Stack ATOM Lite) over this protocol. The module
firmware and the protocol's home are the separate repository
[bigonionbots/atom-lite-audio-module](https://github.com/bigonionbots/atom-lite-audio-module)
(`protocol/`); this firmware does not include or build the module firmware.

These files are **verbatim copies** of that repository's `protocol/` directory:

| File | What |
| --- | --- |
| `AudioLinkProtocol.h` | Framing, CRC-16, payloads, the frame parser: the whole wire contract |
| `MuLaw.h` | G.711 µ-law, the live (game sound) codec |
| `Resample2x.h` | The 2:1 / 1:2 low-pass both ends of a live stream use |

Copied from commit `e887c27`. Formatting is off here (`.clang-format`) so the copies stay
byte-identical.

**Changing the protocol:** change it in the module repository first, keeping it backwards
compatible (new fields at the end of a payload; receivers accept payloads at least as long as the
struct they know), then copy the files here unchanged and update the commit above. Never edit
them here.
