#!/usr/bin/env python3
# Makes examples/DEMO.MOD: a short original tune with synthesized samples
# (drums, bass, lead, pad), as a 4-channel ProTracker M.K. module.
#   python3 tools/make_demo_mod.py [out.mod]
import math, random, struct, sys

random.seed(1)
RATE = 8287  # what C-2 (period 428) plays a sample at

def clip(v):
    return max(-128, min(127, int(round(v))))

def kick():
    out, phase = [], 0.0
    for i in range(3000):
        t = i / RATE
        f = 40 + 110 * math.exp(-t * 30)
        phase += 2 * math.pi * f / RATE
        out.append(clip(120 * math.sin(phase) * math.exp(-t * 9)))
    return out

def snare():
    out, phase = [], 0.0
    for i in range(2400):
        t = i / RATE
        phase += 2 * math.pi * 190 / RATE
        v = 55 * math.sin(phase) * math.exp(-t * 25) + 70 * random.uniform(-1, 1) * math.exp(-t * 14)
        out.append(clip(v))
    return out

def hihat():
    out, prev = [], 0.0
    for i in range(700):
        n = random.uniform(-1, 1)
        hp = n - prev  # crude high-pass
        prev = n
        out.append(clip(60 * hp * math.exp(-i / RATE * 40)))
    return out

def cycle(fn, n, amp):
    return [clip(amp * fn(i / n)) for i in range(n)]

saw = lambda x: 2 * x - 1
square = lambda x: 1 if x < 0.5 else -1
tri = lambda x: 4 * x - 1 if x < 0.5 else 3 - 4 * x

samples = [  # name, data, volume, loop (start, length) in bytes or None
    ("kick", kick(), 64, None),
    ("snare", snare(), 52, None),
    ("hihat", hihat(), 40, None),
    ("bass (saw)", cycle(saw, 64, 100), 48, (0, 64)),
    ("lead (square)", cycle(square, 32, 70), 40, (0, 32)),
    ("pad (triangle)", cycle(tri, 32, 110), 32, (0, 32)),
]

# Periods for notes C-1..B-3 (finetune 0).
PERIODS = [856, 808, 762, 720, 678, 640, 604, 570, 538, 508, 480, 453,
           428, 404, 381, 360, 339, 320, 302, 285, 269, 254, 240, 226,
           214, 202, 190, 180, 170, 160, 151, 143, 135, 127, 120, 113]
NAMES = ["C-", "C#", "D-", "D#", "E-", "F-", "F#", "G-", "G#", "A-", "A#", "B-"]

def note(name):  # "A-2" -> period
    return PERIODS[NAMES.index(name[:2]) + 12 * (int(name[2]) - 1)]

def cell(period=0, sample=0, effect=0, param=0):
    return bytes([(sample & 0xf0) | (period >> 8), period & 0xff, ((sample & 15) << 4) | effect, param])

EMPTY = cell()

def pattern(rows):  # rows: list of 64 lists of 4 cells
    return b"".join(b"".join(r) for r in rows)

def blank():
    return [[EMPTY] * 4 for _ in range(64)]

# Chords per bar (4 bars of 16 rows per pattern): Am F C G, then Dm F C E
CHORDS_A = [("A-1", 0x37), ("F-1", 0x47), ("C-2", 0x47), ("G-1", 0x47)]
CHORDS_B = [("D-2", 0x37), ("F-1", 0x47), ("C-2", 0x47), ("E-1", 0x37)]

def drums(rows, fill=False):
    for r in range(64):
        beat = r % 16
        if beat in (0, 8) or (fill and r >= 56 and r % 2 == 0):
            rows[r][0] = cell(note("C-2"), 1)
        elif beat in (4, 12):
            rows[r][0] = cell(note("C-2"), 2)
        elif r % 2 == 0:
            rows[r][0] = cell(note("C-3"), 3, 0xC, 0x20 if r % 4 else 0x30)

def bass(rows, chords):
    for bar, (root, _) in enumerate(chords):
        for k, step in enumerate([0, 6, 8, 12, 14]):
            n = root if k != 3 else root[:2] + str(int(root[2]) + 1)
            rows[bar * 16 + step][1] = cell(note(n), 4)

def pad(rows, chords):
    for bar, (root, arp) in enumerate(chords):
        n = root[:2] + str(min(3, int(root[2]) + 1))
        rows[bar * 16][3] = cell(note(n), 6, 0x0, arp)
        for k in range(1, 16):  # keep the arpeggio going
            rows[bar * 16 + k][3] = cell(0, 0, 0x0, arp)

MELODY_A = ["E-3", None, "C-3", None, "A-2", None, "C-3", "E-3", "F-3", None, "E-3", None, "C-3", None, None, None,
            "A-2", None, "C-3", None, "F-3", None, "E-3", "C-3", "E-3", None, None, None, "G-3", None, "F-3", None,
            "E-3", None, "G-3", None, "C-3", None, "E-3", None, "G-3", None, "E-3", None, "C-3", None, None, None,
            "B-2", None, "D-3", None, "G-3", None, "F-3", "D-3", "B-2", None, None, None, None, None, None, None]

def melody(rows, notes, vibrato=False):
    for r, n in enumerate(notes):
        if n:
            rows[r][2] = cell(note(n), 5, 0x4 if vibrato else 0, 0x34 if vibrato else 0)

def make():
    patterns = []
    p = blank(); drums(p); bass(p, CHORDS_A); pad(p, CHORDS_A)        # 0: intro
    p[0][0] = cell(note("C-2"), 1, 0xF, 6)
    patterns.append(p)
    p = blank(); drums(p); bass(p, CHORDS_A); pad(p, CHORDS_A); melody(p, MELODY_A)  # 1
    patterns.append(p)
    p = blank(); drums(p, fill=True); bass(p, CHORDS_B); pad(p, CHORDS_B)  # 2: variation
    melody(p, [n and (n[:2] + str(min(3, int(n[2])))) for n in MELODY_A[32:] + MELODY_A[:32]], vibrato=True)
    patterns.append(p)
    p = blank(); drums(p); bass(p, CHORDS_A); pad(p, CHORDS_A); melody(p, MELODY_A)  # 3: ending
    for r in range(48, 64):
        p[r][3] = cell(0, 0, 0xA, 0x02)  # fade the pad
    p[63][2] = cell(0, 0, 0xB, 0x00)     # back to the start
    patterns.append(p)
    orders = [0, 1, 1, 2, 3]
    return patterns, orders

def write(path):
    patterns, orders = make()
    head = b"PICO DEMO".ljust(20, b"\0")
    data = b""
    for i in range(31):
        if i < len(samples):
            name, d, vol, loop = samples[i]
            if len(d) % 2:
                d = d + [0]
            ls, ll = loop if loop else (0, 2)
            head += name.encode().ljust(22, b"\0") + struct.pack(">HBBHH", len(d) // 2, 0, vol, ls // 2, ll // 2)
            data += bytes(v & 0xff for v in d)
        else:
            head += b"\0" * 22 + struct.pack(">HBBHH", 0, 0, 0, 0, 1)
    head += bytes([len(orders), 127]) + bytes(orders).ljust(128, b"\0") + b"M.K."
    body = b"".join(pattern(p) for p in patterns)
    open(path, "wb").write(head + body + data)
    print(path, len(head + body + data), "bytes")

write(sys.argv[1] if len(sys.argv) > 1 else "examples/DEMO.MOD")
