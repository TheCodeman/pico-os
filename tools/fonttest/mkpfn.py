#!/usr/bin/env python3
"""Makes a PFN font (pre-drawn 4-bit anti-aliased glyphs, for the font
command; the format is in src/font/fnt.c) from a TrueType or OpenType font,
drawn by FreeType (Pillow), with its kerning (from raqm's layout).

    tools/fonttest/mkpfn.py FONT.ttf OUT.PFN [sizes...]   (default 12 16 24 32 48)

Characters: ASCII 32-126 and Latin-1 160-255 that the font has.

Copyright (c) 2026 Ken Segler. SPDX-License-Identifier: MIT
"""
import struct
import sys

from PIL import ImageFont, features


def strike(path, px, chars):
    """One size: (header fields, glyph table, pairs, pixels)."""
    engine = ImageFont.Layout.RAQM if features.check('raqm') else ImageFont.Layout.BASIC
    font = ImageFont.truetype(path, size=px, layout_engine=engine)
    asc, desc = font.getmetrics()
    single = ['-kern', '-liga'] if engine == ImageFont.Layout.RAQM else None
    pair = ['-liga'] if engine == ImageFont.Layout.RAQM else None
    glyphs, bits, adv = [], bytearray(), {}
    for c in chars:
        ch = chr(c)
        a = font.getlength(ch, features=single)
        adv[c] = a
        mask, (ox, oy) = font.getmask2(ch, mode='L', anchor='ls', features=single)
        bb = mask.getbbox()  # without the blank edges
        w, h = (bb[2] - bb[0], bb[3] - bb[1]) if bb else (0, 0)
        if bb:
            ox, oy = ox + bb[0], oy + bb[1]
        if w > 255 or h > 255:
            sys.exit(f'{px} px is too big for PFN (a glyph is {w}x{h})')
        off = len(bits)
        for y in range(h):
            row = [mask.getpixel((bb[0] + x, bb[1] + y)) for x in range(w)] + [0]
            for x in range(0, w, 2):
                bits.append((row[x] + 8) // 17 << 4 | (row[x + 1] + 8) // 17)
        glyphs.append(struct.pack('<HBBbbHI', c, w, h, ox, oy, round(a * 64), off))
    pairs = []
    if pair is not None:  # the kerning: a pair's length less its two letters'
        ascii_ = [c for c in chars if c < 127]
        for a in ascii_:
            for b in ascii_:
                k = round((font.getlength(chr(a) + chr(b), features=pair) - adv[a] - adv[b]) * 64)
                if k:
                    pairs.append(struct.pack('<HHh', a, b, k))
    return (px, asc, desc, 0, len(glyphs), len(pairs)), glyphs, pairs, bytes(bits)


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    path, out = sys.argv[1], sys.argv[2]
    sizes = [int(s) for s in sys.argv[3:]] or [12, 16, 24, 32, 48]
    font = ImageFont.truetype(path, size=16)
    chars = []
    for c in list(range(32, 127)) + list(range(160, 256)):
        try:
            if c < 127 or font.getmask(chr(c)).getbbox() or c == 160:
                chars.append(c)
        except Exception:
            pass
    parts = [strike(path, px, chars) for px in sizes]
    head = b'PFN1' + struct.pack('<HH', len(parts), 4)
    pos = len(head) + 4 * len(parts)
    offs, body = [], bytearray()
    for (hdr, glyphs, pairs, bits) in parts:
        so = pos + len(body)
        go = so + 24
        ko = go + 12 * len(glyphs)
        bo = ko + 6 * len(pairs)
        body += struct.pack('<HhhhHHIII', *hdr, go, ko, bo)
        body += b''.join(glyphs) + b''.join(pairs) + bits
        offs.append(so)
        print(f'{hdr[0]:3} px: {hdr[4]} glyphs, {hdr[5]} pairs, {24 + 12 * len(glyphs) + 6 * len(pairs) + len(bits)} bytes')
    data = head + b''.join(struct.pack('<I', o) for o in offs) + body
    open(out, 'wb').write(data)
    print(f'{out}: {len(data)} bytes')


if __name__ == '__main__':
    main()
