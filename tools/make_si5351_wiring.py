#!/usr/bin/env python3
# Draws docs/si5351_wiring.png: a Si5351A module on the board's I2C1
# (GP2 SDA, GP3 SCL), powered from 3.3 V.
#   python3 tools/make_si5351_wiring.py
import os
from PIL import Image, ImageDraw, ImageFont

W, H = 900, 470
img = Image.new('RGB', (W, H), 'white')
d = ImageDraw.Draw(img)
F = '/usr/share/fonts/truetype/dejavu/'
font = ImageFont.truetype(F + 'DejaVuSans.ttf', 17)
bold = ImageFont.truetype(F + 'DejaVuSans-Bold.ttf', 19)
small = ImageFont.truetype(F + 'DejaVuSans.ttf', 14)
mono = ImageFont.truetype(F + 'DejaVuSansMono.ttf', 16)

# The board (left) and the module (right).
bx, by, bw, bh = 40, 50, 300, 320
mx, my, mw, mh = 580, 70, 280, 280
d.rounded_rectangle((bx, by, bx + bw, by + bh), 14, fill='#1f6b3a', outline='#0d3b1f', width=3)
d.text((bx + 16, by + 12), 'Pico 2 mini OS board', font=bold, fill='white')
d.text((bx + 16, by + 38), '(Pimoroni Pico Plus 2, Pico pinout)', font=small, fill='#d8f0de')
d.rounded_rectangle((mx, my, mx + mw, my + mh), 14, fill='#5b2a86', outline='#2e1347', width=3)
d.text((mx + 16, my + 12), 'Si5351A module', font=bold, fill='white')
d.text((mx + 16, my + 38), '25 MHz crystal, I2C 0x60', font=small, fill='#eadcf7')

# Wires: label at the board (with its physical pin), label at the module.
wires = [
    ('3V3 (OUT)', 'pin 36', 'VIN / VCC', '#d62728', '3.3 V'),
    ('GND', 'pin 3 or 8', 'GND', '#222222', 'ground'),
    ('GP2  SDA', 'pin 4', 'SDA', '#1f77b4', 'I2C1 data'),
    ('GP3  SCL', 'pin 5', 'SCL', '#e6a700', 'I2C1 clock'),
]
for i, (bl, pin, ml, col, what) in enumerate(wires):
    y = by + 100 + i * 52
    ym = my + 90 + i * 40
    # pads
    d.rectangle((bx + bw - 14, y - 8, bx + bw + 2, y + 8), fill='#d4af37', outline='#7a6000')
    d.rectangle((mx - 2, ym - 8, mx + 14, ym + 8), fill='#d4af37', outline='#7a6000')
    d.text((bx + 16, y - 11), bl, font=mono, fill='white')
    d.text((bx + 150, y - 9), pin, font=small, fill='#d8f0de')
    d.text((mx + 24, ym - 11), ml, font=mono, fill='white')
    # the wire, with a bend
    mid = (bx + bw + mx) // 2 + (i - 1.5) * 18
    d.line([(bx + bw + 2, y), (mid, y), (mid, ym), (mx - 2, ym)], fill=col, width=5, joint='curve')
    d.text((bx + bw + 14, y - 26), what, font=small, fill=col)

# The outputs.
for i in range(3):
    ox = mx + 30 + i * 80
    oy = my + mh
    d.rectangle((ox, oy - 2, ox + 40, oy + 26), fill='#c9a227', outline='#6b5200')
    d.text((ox - 2, oy + 32), 'CLK%d' % i, font=mono, fill='#333')
d.text((mx, my + mh + 58), 'outputs: SMA or pins', font=small, fill='#444')

d.text((40, 440), 'Most modules have pull-ups on SDA/SCL and take 3.3 V. Use 3.3 V, not 5 V: GP2/GP3 are 3.3 V pins.',
       font=small, fill='#333')
img.save(os.path.join(os.path.dirname(__file__), '..', 'docs', 'si5351_wiring.png'))
