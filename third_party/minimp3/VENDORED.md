# minimp3

`minimp3.h` and `LICENSE` from [lieff/minimp3](https://github.com/lieff/minimp3) at commit
`a2ad968c94819fb98589cca8026e0a57958c963c` (2026-11-15), unmodified. CC0 1.0 (public domain).

Used by `src/audio/PcmDecoder.cpp` (USB headphones, `t5s3_audio` only), which includes the header
with `MINIMP3_IMPLEMENTATION` and `MINIMP3_ONLY_MP3`. Checked against ffmpeg by
`test/run_audio_decode_host_test.sh`.
