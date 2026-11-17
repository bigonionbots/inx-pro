# opencore-aac (PacketVideo AAC decoder)

The `pv` AAC decoder from Android's `platform/frameworks/av`, `media/libstagefright/codecs/aacdec`,
at tag `android-4.1.2_r2.1`: the last release that shipped it (Android uses Fraunhofer's decoder
since). Apache-2.0 (`NOTICE`, `MODULE_LICENSE_APACHE2`).

- `src/`: every `.cpp` and `.h` of that directory, unmodified, except Android's OMX wrappers
  (`SoftAAC.*`, `SoftAAC2.*`) and `Android.mk`, which were left out.
- `library.json` (not from Android): builds it for the `t5s3_audio` env with the defines of the
  `pv` branch of `Android.mk` (`AAC_PLUS`, `HQ_SBR`, `PARAMETRICSTEREO`, empty `OSCL_*` macros),
  so AAC-LC, HE-AAC and HE-AAC v2 decode. The portable C fixed-point path is used
  (`fxp_mul32_c_equivalent.h`). `-Wno-narrowing` lets its hex tables compile as C++11 or later.

Used by `src/audio/PcmDecoder.cpp` (USB headphones). Checked against ffmpeg by
`test/run_audio_decode_host_test.sh`.
