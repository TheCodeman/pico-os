#!/bin/sh
# Builds the IRC client for the PC, runs its checks against a pretend
# server, and writes its screenshots (irc_*.png) into the folder given
# (default: here).
#   tools/irctest/uitest.sh [out-folder]
set -e
cd "$(dirname "$0")"
OUT=$(cd "${1:-.}" && pwd)
T=$(mktemp -d)
SRC=../../src
cc -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -Wno-missing-field-initializers \
    -fsanitize=address,undefined -DIRC_PC_TEST -Istubs -I../hextest/stubs -I../si5351test/stubs -I$SRC \
    -I$SRC/net -I$SRC/www uitest.c $SRC/www/www_html.c -o "$T/uitest"
(cd "$T" && ./uitest)
for s in connect main menu popup server help commands; do
    python3 - "$T/shot_$s.raw" "$OUT/irc_$s.png" <<'P'
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
