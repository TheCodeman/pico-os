#!/bin/sh
# Builds the si5351 panel for the PC against a simulated chip and writes its
# screenshots (si5351_*.png) into the folder given (default: here).
#   tools/si5351test/uitest.sh [out-folder]
set -e
cd "$(dirname "$0")"
OUT=$(cd "${1:-.}" && pwd)
T=$(mktemp -d)
cc -O1 -Wall -Wno-unused-function -Istubs -I../../src/si5351 -I../../src uitest.c ../../src/si5351/si5351.c -lm -o "$T/uitest"
(cd "$T" && ./uitest)
for s in main iq entry mem none quit; do
    python3 - "$T/shot_$s.raw" "$OUT/si5351_$s.png" <<'P'
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
cat "$T/SI5351.INI"
rm -rf "$T"
