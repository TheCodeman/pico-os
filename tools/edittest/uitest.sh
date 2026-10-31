#!/bin/sh
# Builds the text editor for the PC, runs its checks, and writes its
# screenshots (edit_*.png) into the folder given (default: here).
#   tools/edittest/uitest.sh [out-folder]
set -e
cd "$(dirname "$0")"
OUT=$(cd "${1:-.}" && pwd)
T=$(mktemp -d)
cc -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -Wno-format-truncation -fsanitize=address,undefined \
    -Istubs -I../../src uitest.c -o "$T/uitest"
(cd "$T" && ./uitest)
for s in menu drag select bar; do
    python3 - "$T/shot_$s.raw" "$OUT/edit_$s.png" <<'P'
import sys, zlib, struct
d = open(sys.argv[1], 'rb').read()
raw = bytearray()
for y in range(480):
    raw.append(0)
    raw += d[y * 640 * 3:(y + 1) * 640 * 3]
def chunk(t, b): return struct.pack('>I', len(b)) + t + b + struct.pack('>I', zlib.crc32(t + b) & 0xffffffff)
open(sys.argv[2], 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', 640, 480, 8, 2, 0, 0, 0))
                              + chunk(b'IDAT', zlib.compress(bytes(raw))) + chunk(b'IEND', b''))
P
done
rm -rf "$T"
