#!/bin/sh
# src/mpaprobe.h: the MP3 AmigaMCE times its MPEG audio decoder with
# (stream.c's mpa_measure): 14 frames, a third of a second, of 128 kbit/s
# joint stereo at 44.1 kHz, as most MP3s are. Synthetic (a chord that
# moves, its two sides apart, and some noise), so the program carries no
# one's music. Needs ffmpeg with libmp3lame. MIT, (c) 2026 Brian King.
set -e
out=${1:-src/mpaprobe.h}
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
ffmpeg -v error -f lavfi -i "aevalsrc=exprs='0.25*sin(2*PI*220*t)*(1+0.5*sin(2*PI*3*t))+0.18*sin(2*PI*277.2*t+3*sin(2*PI*5*t))+0.12*sin(2*PI*1318.5*t)+0.06*(random(0)-0.5)|0.25*sin(2*PI*329.6*t)*(1+0.5*sin(2*PI*2*t))+0.18*sin(2*PI*440*t+2*sin(2*PI*7*t))+0.1*sin(2*PI*2637*t)+0.06*(random(1)-0.5)':s=44100:d=0.4" \
    -c:a libmp3lame -b:a 128k -joint_stereo 1 -write_xing 0 -id3v2_version 0 -f mp3 "$tmp/p.mp3"
python3 - "$tmp/p.mp3" "$out" <<'PY'
import sys
d = open(sys.argv[1], 'rb').read()
# the first 14 frames (128 kbit/s at 44.1 kHz: 417 or 418 bytes each)
i, n = 0, 0
while n < 14:
    h = d[i:i + 4]
    assert h[0] == 0xFF and (h[1] & 0xE0) == 0xE0, 'not a frame at %d' % i
    i += 144 * 128000 // 44100 + ((h[2] >> 1) & 1)
    n += 1
d = d[:i]
with open(sys.argv[2], 'w') as f:
    f.write('/* src/mpaprobe.h: made by tools/mkmpaprobe.sh (synthetic sound; MIT) */\n')
    f.write('static const UBYTE mpa_probe[%d] = {\n' % len(d))
    for k in range(0, len(d), 16):
        f.write('    ' + ', '.join('0x%02x' % b for b in d[k:k + 16]) + ',\n')
    f.write('};\n#define MPA_PROBE_FRAMES 14\n')
print(len(d), 'bytes')
PY
