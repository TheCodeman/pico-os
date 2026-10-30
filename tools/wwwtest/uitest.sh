#!/bin/sh
# Builds the web browser for the PC, runs its checks against a pretend web
# server, and writes its screenshots (www_*.png) into the folder given
# (default: here).
#   tools/wwwtest/uitest.sh [/absolute/out-folder]
set -e
cd "$(dirname "$0")"
OUT=$(cd "${1:-.}" && pwd)
T=$(mktemp -d)
SRC=../../src
FLAGS="-O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -Wno-format-truncation -fsanitize=address,undefined -include stubs/pc_printf.h -Istubs -I$SRC -I$SRC/mc -I$SRC/net -I$SRC/www"
for f in $SRC/mc/mc_scr.c $SRC/mc/mc_ui.c $SRC/www/www_html.c $SRC/www/www_net.c $SRC/www/www.c uitest.c; do
    cc $FLAGS -I$SRC/image -c "$f" -o "$T/$(basename "$f" .c).o"
done
for f in image lodepng tjpgd; do  # the picture viewer
    cc -O1 -w -fsanitize=address,undefined -include stubs/pc_printf.h -Istubs -I$SRC -I$SRC/image -c $SRC/image/$f.c -o "$T/$f.o"
done
cc -fsanitize=address,undefined "$T"/*.o -o "$T/uitest"
cc -O1 -g -Wall -Wextra -fsanitize=address,undefined -Istubs -I$SRC/www htmltest.c $SRC/www/www_html.c -o "$T/htmltest"
"$T/htmltest"
(cd "$T" && ./uitest)
for s in home demo menu field results find info help popup; do
    python3 - "$T/shot_$s.raw" "$OUT/www_$s.png" <<'P'
import sys, zlib, struct
d = open(sys.argv[1], 'rb').read()
raw = bytearray()
for y in range(480):
    raw.append(0)
    raw += d[y * 1920:(y + 1) * 1920]
def chunk(t, b): return struct.pack('>I', len(b)) + t + b + struct.pack('>I', zlib.crc32(t + b) & 0xffffffff)
open(sys.argv[2], 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', 640, 480, 8, 2, 0, 0, 0))
                              + chunk(b'IDAT', zlib.compress(bytes(raw))) + chunk(b'IEND', b''))
P
done
rm -rf "$T"
