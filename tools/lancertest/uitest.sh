#!/bin/sh
# Builds Lancer for the PC, lets its autopilot play a game through (three
# stages and their bosses, the pod and its weapons, a second round, game
# over, high-score entry), runs the checks, and writes the screenshots
# (lancer_*.png, 640x480 as on the HDMI screen), a picture of each stage's
# map (lancer_map_N.png, enemies in red, restart points in green) and the
# sounds (lancer_sounds.wav) into the folder given (default: here).
#   tools/lancertest/uitest.sh [out-folder]
set -e
cd "$(dirname "$0")"
OUT=$(cd "${1:-.}" && pwd)
T=$(mktemp -d)
cc -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -fsanitize=address,undefined -Istubs \
    -I../swarmtest/stubs -I../spritetest/stubs -I../../src/lancer -I../../src/sprite -I../../src/image -I../../src \
    uitest.c ../../src/lancer/lancer_snd.c ../../src/sprite/spr.c ../../src/image/lodepng.c -lm -o "$T/uitest"
status=0
(cd "$T" && ./uitest) || status=$?
for f in "$T"/shot_*.raw "$T"/map_*.raw; do
    [ -e "$f" ] || continue
    python3 - "$f" "$OUT" <<'P'
import os, re, sys, zlib, struct
src, out = sys.argv[1], sys.argv[2]
name = os.path.basename(src)[:-4]
d = open(src, 'rb').read()
m = re.match(r'map_(\d+)_(\d+)x(\d+)', name)
if m:
    w, h, scale, dst = int(m.group(2)), int(m.group(3)), 1, 'lancer_map_%s.png' % m.group(1)
else:
    w, h, scale, dst = 320, 240, 2, 'lancer_%s.png' % name[5:]
raw = bytearray()
for y in range(h * scale):
    raw.append(0)
    for v in d[(y // scale) * w:(y // scale + 1) * w]:
        px = bytes(((v >> 5) * 255 // 7, ((v >> 2) & 7) * 255 // 7, (v & 3) * 85))
        raw += px * scale
def chunk(t, b): return struct.pack('>I', len(b)) + t + b + struct.pack('>I', zlib.crc32(t + b) & 0xffffffff)
open(os.path.join(out, dst), 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w * scale, h * scale, 8, 2, 0, 0, 0))
                                + chunk(b'IDAT', zlib.compress(bytes(raw))) + chunk(b'IEND', b''))
P
done
cp "$T/sounds.wav" "$OUT/lancer_sounds.wav"
rm -rf "$T"
exit $status
