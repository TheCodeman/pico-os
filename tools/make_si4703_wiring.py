#!/usr/bin/env python3
# Draws docs/fm_wiring.png: a Si4703 FM module on the board's I2C1
# (GP2 SDIO, GP3 SCLK) with its reset on GP22, powered from 3.3 V.
#   python3 tools/make_si4703_wiring.py
import os
from PIL import Image, ImageDraw, ImageFont

W, H = 900, 560
img = Image.new('RGB', (W, H), 'white')
d = ImageDraw.Draw(img)
F = '/usr/share/fonts/truetype/dejavu/'
font = ImageFont.truetype(F + 'DejaVuSans.ttf', 17)
bold = ImageFont.truetype(F + 'DejaVuSans-Bold.ttf', 19)
small = ImageFont.truetype(F + 'DejaVuSans.ttf', 14)
mono = ImageFont.truetype(F + 'DejaVuSansMono.ttf', 16)

# The board (left) and the module (right).
bx, by, bw, bh = 40, 50, 300, 340
mx, my, mw, mh = 580, 60, 280, 390
d.rounded_rectangle((bx, by, bx + bw, by + bh), 14, fill='#1f6b3a', outline='#0d3b1f', width=3)
d.text((bx + 16, by + 12), 'Pico 2 mini OS board', font=bold, fill='white')
d.text((bx + 16, by + 38), '(Pimoroni Pico Plus 2, Pico pinout)', font=small, fill='#d8f0de')
d.rounded_rectangle((mx, my, mx + mw, my + mh), 14, fill='#5b2a86', outline='#2e1347', width=3)
d.text((mx + 16, my + 12), 'Si4703 FM module', font=bold, fill='white')
d.text((mx + 16, my + 38), '(SparkFun-style breakout) I2C 0x10', font=small, fill='#eadcf7')

# Wires: label at the board (with its physical pin), label at the module.
wires = [
    ('3V3 (OUT)', 'pin 36', '3.3V', '#d62728', '3.3 V'),
    ('GND', 'pin 3 or 8', 'GND', '#222222', 'ground'),
    ('GP2  SDA', 'pin 4', 'SDIO', '#1f77b4', 'I2C1 data'),
    ('GP3  SCL', 'pin 5', 'SCLK', '#e6a700', 'I2C1 clock'),
    ('GP22', 'pin 29', 'RST', '#2ca02c', 'reset'),
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

# The pins left unconnected, and the audio.
for i, name in enumerate(['SEN', 'GPIO1', 'GPIO2']):
    d.text((mx + 24, my + 90 + (5 + i) * 40 - 11), name, font=mono, fill='#cbb6e6')
d.text((mx + 110, my + 90 + 5 * 40 - 9), '(not connected)', font=small, fill='#eadcf7')
d.rounded_rectangle((mx + 70, my + mh - 4, mx + 210, my + mh + 30), 8, fill='#333', outline='#111')
d.text((mx + 82, my + mh + 4), 'headphones', font=small, fill='white')
d.text((mx, my + mh + 40), 'audio and aerial: the headphone cable', font=small, fill='#444')

d.text((40, 530), 'Use 3.3 V, not 5 V. The reset pin picks I2C mode as it rises (SDIO held low); GP2/GP3 can share a Si5351.',
       font=small, fill='#333')
img.save(os.path.join(os.path.dirname(__file__), '..', 'docs', 'fm_wiring.png'))
