#!/bin/sh
# Builds the SDR receiver for the PC with a pretend dongle, runs its checks
# and the signal processing's (dsptest.c), and writes its screenshots
# (sdr_*.png) into the folder given (default: here).
#   tools/sdrtest/uitest.sh [out-folder]
set -e
cd "$(dirname "$0")"
OUT=$(cd "${1:-.}" && pwd)
T=$(mktemp -d)
FLAGS="-O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -fsanitize=address,undefined"
cc $FLAGS -I../../src/sdr dsptest.c ../../src/sdr/sdr_dsp.c -lm -o "$T/dsptest"
cc $FLAGS -Istubs -I../wavtest/stubs -I../si5351test/stubs -I../../src/sdr -I../../src uitest.c \
    ../../src/sdr/sdr_dsp.c -lm -o "$T/uitest"
"$T/dsptest"
(cd "$T" && ./uitest)
for s in main nfm favs help nodongle; do
    python3 - "$T/shot_$s.raw" "$OUT/sdr_$s.png" <<'P'
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
rm -rf "$T"
