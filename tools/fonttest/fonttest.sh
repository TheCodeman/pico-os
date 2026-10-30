#!/bin/sh
# Builds src/font/fnt.c for the PC (ASan) and runs fonttest.c on fonts:
# a PFN file is made from the first with mkpfn.py, each font's sample page is
# written as font_N.png into the folder given (default: here).
#   tools/fonttest/fonttest.sh FONT.ttf [FONT.otf ...] [-o out-folder]
set -e
cd "$(dirname "$0")"
OUT=.
FONTS=""
while [ $# -gt 0 ]; do
    case "$1" in
    -o) OUT="$2"; shift 2 ;;
    *) FONTS="$FONTS $(cd "$(dirname "$1")" && pwd)/$(basename "$1")"; shift ;;
    esac
done
[ -n "$FONTS" ] || { echo "usage: $0 FONT.ttf [FONT.otf ...] [-o out]"; exit 1; }
OUT=$(cd "$OUT" && pwd)
T=$(mktemp -d)
SRC=../../src
FLAGS="-O1 -g -Wall -Wextra -Wno-unused-parameter -fsanitize=address,undefined -I../mdtest/stubs -I$SRC/font"
cc $FLAGS -c fonttest.c -o "$T/fonttest.o"
cc $FLAGS -c $SRC/font/fnt.c -o "$T/fnt.o"
cc -O2 -w -fsanitize=address,undefined -I../mdtest/stubs -I$SRC/font -c $SRC/font/fnt_stb.c -o "$T/fnt_stb.o"
cc -fsanitize=address,undefined "$T"/*.o -lm -o "$T/fonttest"
python3 mkpfn.py $(echo $FONTS | cut -d' ' -f1) "$T/TEST.PFN" > /dev/null
(cd "$T" && ./fonttest $FONTS TEST.PFN)
i=1
for f in $FONTS TEST.PFN; do
    python3 - "$T/shot_$i.raw" "$OUT/font_$i.png" <<'P'
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
    i=$((i + 1))
done
rm -rf "$T"
