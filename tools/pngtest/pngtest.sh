#!/bin/sh
# Builds src/image (png_rows.c and LodePNG) for the PC with ASan and checks
# png_rows_decode against LodePNG on PNGs of every kind, made with Pillow.
#   tools/pngtest/pngtest.sh
set -e
cd "$(dirname "$0")"
T=$(mktemp -d)
SRC=../../src
FLAGS="-O1 -g -fsanitize=address,undefined -I$SRC/image -I../mdtest/stubs"
cc $FLAGS -Wall -Wextra -c pngtest.c -o "$T/pngtest.o"
cc $FLAGS -Wall -Wextra -c $SRC/image/png_rows.c -o "$T/png_rows.o"
for f in image lodepng tjpgd; do
    cc $FLAGS -w -c $SRC/image/$f.c -o "$T/$f.o"
done
cc -fsanitize=address,undefined "$T"/*.o -o "$T/pngtest"
python3 - "$T" <<'P'
import sys, random, zlib
from PIL import Image, ImageDraw
t = sys.argv[1]
random.seed(1)
def picture(w, h, mode):
    im = Image.new('RGB', (w, h))
    d = ImageDraw.Draw(im)
    for y in range(0, h, 3):
        d.line([(0, y), (w, y)], fill=(y * 255 // h, random.randrange(256), (w - y) % 256))
    for _ in range(12):
        x, y = random.randrange(w), random.randrange(h)
        d.ellipse([x, y, x + random.randrange(1, w + 1), y + random.randrange(1, h + 1)],
                  fill=tuple(random.randrange(256) for _ in range(3)))
    d.text((3, 3), "png_rows", fill=(255, 255, 255))
    return im if mode == 'RGB' else im.convert(mode)
n = 0
for (w, h) in [(1, 1), (5, 3), (17, 9), (64, 48), (333, 201), (640, 480)]:
    for mode in ['RGB', 'L', '1', 'P']:
        for level in [0, 1, 6, 9]:
            im = picture(w, h, mode)
            im.save(f'{t}/p{n}.png', compress_level=level, optimize=False)
            n += 1
    # 16-bit grey, low-bit palettes, with alpha (left to LodePNG).
    im = picture(w, h, 'RGB')
    im.convert('L').convert('I').point(lambda v: v * 257).convert('I;16').save(f'{t}/p{n}.png'); n += 1
    im.quantize(4).save(f'{t}/p{n}.png', bits=2); n += 1
    im.quantize(16).save(f'{t}/p{n}.png', bits=4); n += 1
    im.convert('RGBA').save(f'{t}/p{n}.png'); n += 1
    im.convert('LA').save(f'{t}/p{n}.png'); n += 1
    q = im.quantize(8)
    q.info['transparency'] = 3
    q.save(f'{t}/p{n}.png', transparency=3); n += 1
    # 16-bit RGB, grey 2 and 4 bits: raw PNGs made here.
    import struct
    def chunk(ty, b): return struct.pack('>I', len(b)) + ty + b + struct.pack('>I', zlib.crc32(ty + b) & 0xffffffff)
    def raw_png(path, depth, ctype, rowbytes_fn, wbits=15):
        raw = bytearray()
        for y in range(h):
            raw.append(random.randrange(5))
            raw += bytes(random.randrange(256) for _ in range(rowbytes_fn()))
        co = zlib.compressobj(6, zlib.DEFLATED, wbits)
        z = co.compress(bytes(raw)) + co.flush()
        mid = len(z) // 2
        open(path, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, depth, ctype, 0, 0, 0))
                               + chunk(b'IDAT', z[:mid]) + chunk(b'IDAT', z[mid:]) + chunk(b'IEND', b''))
    raw_png(f'{t}/p{n}.png', 16, 2, lambda: w * 6); n += 1
    raw_png(f'{t}/p{n}.png', 2, 0, lambda: (w * 2 + 7) // 8); n += 1
    raw_png(f'{t}/p{n}.png', 4, 0, lambda: (w * 4 + 7) // 8, 9); n += 1
    raw_png(f'{t}/p{n}.png', 8, 2, lambda: w * 3, 10); n += 1
P
"$T/pngtest" "$T"/p*.png
rm -rf "$T"
