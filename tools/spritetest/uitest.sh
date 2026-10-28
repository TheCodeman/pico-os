#!/bin/sh
# Builds the sprite editor for the PC, runs its checks, and writes its
# screenshots (sprite_*.png), the coin it draws (COIN.SPR) and that coin's PNG
# into the folder given (default: here).
#   tools/spritetest/uitest.sh [out-folder]
set -e
cd "$(dirname "$0")"
OUT=$(cd "${1:-.}" && pwd)
T=$(mktemp -d)
cc -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -fsanitize=address,undefined -Istubs \
    -I../si5351test/stubs -I../../src/sprite -I../../src/image -I../../src uitest.c dirstub.c ../../src/sprite/spr.c \
    ../../src/image/lodepng.c -o "$T/uitest"
(cd "$T" && ./uitest)
for s in main load ask size help; do
    python3 - "$T/shot_$s.raw" "$OUT/sprite_$s.png" <<'P'
import sys, zlib, struct
d = open(sys.argv[1], 'rb').read()
raw = bytearray()
for y in range(480):
    raw.append(0)
    for v in d[y * 640:(y + 1) * 640]:
        raw += bytes(((v >> 5) * 255 // 7, ((v >> 2) & 7) * 255 // 7, (v & 3) * 85))
def chunk(t, b): return struct.pack('>I', len(b)) + t + b + struct.pack('>I', zlib.crc32(t + b) & 0xffffffff)
open(sys.argv[2], 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', 640, 480, 8, 2, 0, 0, 0))
                              + chunk(b'IDAT', zlib.compress(bytes(raw))) + chunk(b'IEND', b''))
P
done
# The exported PNG must read back in another program: check its CRCs and data.
python3 - "$T/COIN.PNG" <<'P'
import sys, zlib, struct
d = open(sys.argv[1], 'rb').read()
assert d[:8] == b'\x89PNG\r\n\x1a\n'
i, idat = 8, b''
while i < len(d):
    n, = struct.unpack('>I', d[i:i + 4]); t = d[i + 4:i + 8]; b = d[i + 8:i + 8 + n]
    assert struct.unpack('>I', d[i + 8 + n:i + 12 + n])[0] == zlib.crc32(t + b) & 0xffffffff, t
    if t == b'IHDR': w, h = struct.unpack('>II', b[:8])
    if t == b'IDAT': idat += b
    i += 12 + n
raw = zlib.decompress(idat)
assert len(raw) == h * (1 + 4 * w)
print('COIN.PNG: %dx%d RGBA, CRCs and zlib fine' % (w, h))
P
cp "$T/COIN.SPR" "$T/COIN.PNG" "$OUT/"
rm -rf "$T"
