#!/bin/sh
# Builds Swarm for the PC, lets its autopilot play a game through (stages,
# tractor beam, rescue, challenge stage, game over, high-score entry), runs
# the checks, and writes the screenshots (swarm_*.png, 640x480 as on the
# HDMI screen) and the sounds (swarm_sounds.wav) into the folder given
# (default: here).
#   tools/swarmtest/uitest.sh [out-folder]
set -e
cd "$(dirname "$0")"
OUT=$(cd "${1:-.}" && pwd)
T=$(mktemp -d)
cc -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -fsanitize=address,undefined -Istubs \
    -I../spritetest/stubs -I../../src/swarm -I../../src/sprite -I../../src/image -I../../src uitest.c \
    ../../src/swarm/swarm_snd.c ../../src/sprite/spr.c ../../src/image/lodepng.c -lm -o "$T/uitest"
status=0
(cd "$T" && ./uitest) || status=$?
for f in "$T"/shot_*.raw; do
    s=$(basename "$f" .raw); s=${s#shot_}
    python3 - "$f" "$OUT/swarm_$s.png" <<'P'
import sys, zlib, struct
d = open(sys.argv[1], 'rb').read()
raw = bytearray()
for y in range(480):
    raw.append(0)
    for v in d[(y // 2) * 320:(y // 2 + 1) * 320]:
        px = bytes(((v >> 5) * 255 // 7, ((v >> 2) & 7) * 255 // 7, (v & 3) * 85))
        raw += px + px
def chunk(t, b): return struct.pack('>I', len(b)) + t + b + struct.pack('>I', zlib.crc32(t + b) & 0xffffffff)
open(sys.argv[2], 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', 640, 480, 8, 2, 0, 0, 0))
                              + chunk(b'IDAT', zlib.compress(bytes(raw))) + chunk(b'IEND', b''))
P
done
cp "$T/sounds.wav" "$OUT/swarm_sounds.wav"
rm -rf "$T"
exit $status
