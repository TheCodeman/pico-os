#!/bin/sh
# Builds the Markdown viewer for the PC, runs its checks on a sample page
# with pictures, and writes its screenshots (md_*.png) into the folder
# given (default: here). With MD_FONTS set to a folder of fonts (SS3.TTF,
# SS3-B.TTF, ...), it runs again with them in /FONTS (TrueType text) and
# writes md_ttf_*.png too.
#   [MD_FONTS=folder] tools/mdtest/uitest.sh [out-folder]
set -e
cd "$(dirname "$0")"
OUT=$(cd "${1:-.}" && pwd)
T=$(mktemp -d)
SRC=../../src
STUBS="-Istubs -I../hextest/stubs -I../si5351test/stubs"
FLAGS="-O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-format-truncation -fsanitize=address,undefined $STUBS -I$SRC -I$SRC/md -I$SRC/image -I$SRC/www -I$SRC/font"
cc $FLAGS -Wno-unused-function -c uitest.c -o "$T/uitest.o"
cc $FLAGS -c $SRC/md/md_doc.c -o "$T/md_doc.o"
cc $FLAGS -c $SRC/www/www_html.c -o "$T/www_html.o"
cc $FLAGS -c dirstub.c -o "$T/dirstub.o"
cc $FLAGS -c $SRC/md/md_ttf.c -o "$T/md_ttf.o"
cc $FLAGS -c $SRC/font/fnt.c -o "$T/fnt.o"
cc -O2 -w -fsanitize=address,undefined $STUBS -I$SRC/font -c $SRC/font/fnt_stb.c -o "$T/fnt_stb.o"
for f in image png_rows lodepng tjpgd; do  # the picture decoders
    cc -O1 -w -fsanitize=address,undefined $STUBS -I$SRC -I$SRC/image -c $SRC/image/$f.c -o "$T/$f.o"
done
cc -fsanitize=address,undefined "$T"/*.o -lm -o "$T/uitest"
# The page, another one, and pictures.
D="$T/drive"  # (the shots go beside it)
mkdir -p "$D/sub"
cp sample.md "$D/README.md"
cp OTHER.md "$D/sub/OTHER.md"
python3 - "$D" <<'P'
import sys, zlib, struct
from PIL import Image, ImageDraw
t = sys.argv[1]
im = Image.new('RGB', (240, 120))
d = ImageDraw.Draw(im)
for x in range(240):
    d.line([(x, 0), (x, 119)], fill=(x, 255 - x, 128))
d.ellipse([70, 10, 170, 110], fill=(255, 220, 0), outline=(0, 0, 0))
im.save(t + '/pic.png')
ph = Image.new('RGB', (1024, 768))
d = ImageDraw.Draw(ph)
for y in range(768):
    d.line([(0, y), (1023, y)], fill=(y // 4, 120, 255 - y // 3))
d.rectangle([300, 200, 700, 560], fill=(200, 40, 40))
ph.save(t + '/photo.jpg', quality=85)
big = Image.new('RGB', (900, 300), (30, 90, 200))
ImageDraw.Draw(big).text((20, 20), "big.png", fill=(255, 255, 255))
big.save(t + '/big.png')
P
(cd "$D" && ../uitest)
shots() {  # the shots as PNGs: shots prefix names...
    pre=$1
    shift
    for s in "$@"; do
    python3 - "$T/shot_$s.raw" "$OUT/${pre}_$s.png" <<'P'
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
}
shots md top lists table link toc menu source help open
if [ -n "$MD_FONTS" ]; then  # again, with TrueType text
    mkdir -p "$D/FONTS"
    cp "$MD_FONTS"/SS3*.TTF "$D/FONTS/"
    (cd "$D" && ../uitest)
    shots md_ttf top lists table link help
fi
rm -rf "$T"
