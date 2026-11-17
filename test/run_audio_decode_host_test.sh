#!/usr/bin/env bash
# Host check of PcmDecoder (src/audio/PcmDecoder.h), the T5's MP3 (minimp3) and AAC (opencore-aac)
# decoding for USB headphones. Needs ffmpeg: it encodes test signals, decodes them itself as the
# reference, and each must match ours (fed in random-sized pieces) to the SNR below. Files go
# to build/audio_decode_host/.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$ROOT_DIR/build/audio_decode_host"
BINARY="$BUILD_DIR/audio_decode_host_test"
AAC_DIR="$ROOT_DIR/third_party/opencore_aacdec/src"
mkdir -p "$BUILD_DIR/obj"

# opencore-aac is C-style C++; compile it once per source change.
AAC_FLAGS="-O2 -w -DPCM_DECODER_HOST=1 -DAAC_PLUS -DHQ_SBR -DPARAMETRICSTEREO -DOSCL_IMPORT_REF= -DOSCL_EXPORT_REF= -DOSCL_UNUSED_ARG="
OBJS=()
for src in "$AAC_DIR"/*.cpp; do
  obj="$BUILD_DIR/obj/$(basename "${src%.cpp}").o"
  if [ ! -f "$obj" ] || [ "$src" -nt "$obj" ]; then
    # shellcheck disable=SC2086
    c++ -c $AAC_FLAGS -I"$AAC_DIR" "$src" -o "$obj"
  fi
  OBJS+=("$obj")
done
# shellcheck disable=SC2086
c++ -std=gnu++2a -O2 -Wall -Wextra $AAC_FLAGS -Wno-unused-parameter -I"$AAC_DIR" \
  "$ROOT_DIR/test/audio_decode_host/audio_decode_host_test.cpp" "$ROOT_DIR/src/audio/PcmDecoder.cpp" \
  "$ROOT_DIR/src/audio/Resampler.cpp" \
  "${OBJS[@]}" -o "$BINARY"

cd "$BUILD_DIR"
# A speech-like test signal: a sweep plus a second tone and some noise, 8 s.
signal() { # rate channels
  ffmpeg -loglevel error -y -f lavfi -i "sine=frequency=220:sample_rate=$1:duration=8" \
    -f lavfi -i "aevalsrc=0.3*sin(2*PI*(200+400*t)*t):s=$1:d=8" -f lavfi -i "anoisesrc=r=$1:a=0.02:d=8" \
    -filter_complex "amix=inputs=3:normalize=0,volume=0.8" -ac "$2" -ar "$1" "src_$1_$2.wav"
}

failed=0
check() { # name codec file min_snr expected_rate
  local name=$1 codec=$2 file=$3 min=$4 rate=$5
  local channels
  channels=$(ffprobe -v error -select_streams a:0 -show_entries stream=channels -of csv=p=0 "$file")
  ffmpeg -loglevel error -y -i "$file" -f s16le -ac "$channels" -ar "$rate" "ref_$name.s16"
  local info result snr
  info=$("$BINARY" "$codec" "$file" "out_$name.s16" 7)
  result=$("$BINARY" compare "ref_$name.s16" "out_$name.s16" "$channels")
  snr=$(sed -E 's/.*snr=([-0-9.]+)dB.*/\1/' <<<"$result")
  if grep -q "rate=$rate " <<<"$info" && awk -v s="$snr" -v m="$min" 'BEGIN { exit !(s >= m) }'; then
    echo "PASS $name: $info $result"
  else
    echo "FAIL $name: $info $result (want rate $rate, snr >= $min dB)"
    failed=1
  fi
}

signal 44100 2
signal 22050 1
signal 48000 2
signal 16000 1
ffmpeg -loglevel error -y -i src_44100_2.wav -c:a libmp3lame -b:a 128k -write_xing 0 mp3_44k_stereo.mp3
ffmpeg -loglevel error -y -i src_22050_1.wav -c:a libmp3lame -b:a 32k -write_xing 0 mp3_22k_mono.mp3
ffmpeg -loglevel error -y -i src_16000_1.wav -c:a libmp3lame -b:a 24k -write_xing 0 mp3_16k_mono.mp3
# An ID3 tag in front, as real books have.
ffmpeg -loglevel error -y -i src_44100_2.wav -c:a libmp3lame -b:a 64k -write_xing 0 -id3v2_version 3 \
  -metadata title="Chapter 1" mp3_id3.mp3
ffmpeg -loglevel error -y -i src_44100_2.wav -c:a aac -b:a 128k -f adts aac_44k_stereo.aac
ffmpeg -loglevel error -y -i src_22050_1.wav -c:a aac -b:a 32k -f adts aac_22k_mono.aac
ffmpeg -loglevel error -y -i src_48000_2.wav -c:a aac -b:a 96k -f adts aac_48k_stereo.aac

# Two MP3 decoders agree closely; ffmpeg's float AAC decoder and the fixed-point one less so.
check mp3_44k_stereo mp3 mp3_44k_stereo.mp3 40 44100
check mp3_22k_mono mp3 mp3_22k_mono.mp3 40 22050
check mp3_16k_mono mp3 mp3_16k_mono.mp3 40 16000
check mp3_id3 mp3 mp3_id3.mp3 40 44100
check aac_44k_stereo aac aac_44k_stereo.aac 30 44100
check aac_22k_mono aac aac_22k_mono.aac 30 22050
check aac_48k_stereo aac aac_48k_stereo.aac 30 48000

# Resampler (src/audio/Resampler.h): two tones at the source rate, converted to a rate USB adapters
# take, against the same tones generated at that rate. The filter cuts at 92 % of the lower
# Nyquist frequency, so the tones stay well below it. 437 and 2531 Hz share no period shorter
# than a second, so only the right alignment matches.
tones() { # rate
  ffmpeg -loglevel error -y -f lavfi -i "sine=frequency=437:sample_rate=$1:duration=6" \
    -f lavfi -i "sine=frequency=2531:sample_rate=$1:duration=6" -filter_complex "amix=inputs=2:normalize=0" \
    -ac 2 -f s16le "tones_$1.s16"
}
for pair in 22050:44100 16000:48000 11025:44100 24000:48000 44100:48000 48000:44100 32000:48000 96000:48000; do
  in=${pair%:*} out=${pair#*:}
  [ -f "tones_$in.s16" ] || tones "$in"
  [ -f "tones_$out.s16" ] || tones "$out"
  counts=$("$BINARY" resample "tones_$in.s16" "$in" "$out" "rs_${in}_$out.s16")
  result=$("$BINARY" compare "tones_$out.s16" "rs_${in}_$out.s16" 2)
  snr=$(sed -E 's/.*snr=([-0-9.]+)dB.*/\1/' <<<"$result")
  if awk -v s="$snr" 'BEGIN { exit !(s >= 45) }' && grep -q "offset=0 " <<<"$result"; then
    echo "PASS resample $in -> $out: $counts $result"
  else
    echo "FAIL resample $in -> $out: $counts $result (want snr >= 45 dB at offset 0)"
    failed=1
  fi
done

# Optional HE-AAC samples (put any .aac ADTS files with an "he" name in build/audio_decode_host/).
for f in he*.aac; do
  [ -f "$f" ] || continue
  rate=$(ffprobe -v error -select_streams a:0 -show_entries stream=sample_rate -of csv=p=0 "$f")
  check "${f%.aac}" aac "$f" 20 "$rate"
done

exit $failed
