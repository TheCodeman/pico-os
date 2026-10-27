#!/usr/bin/env python3
# Makes examples/NOVA.MOD, the soundtrack for examples/NOVA.BAS: an original
# 4-channel ProTracker M.K. module with synthesized samples, in four parts
# that NOVA.BAS picks with MUSIC POS:
#   positions 0-1  title theme   (loops on itself)
#   positions 2-6  battle theme  (loops back to position 3)
#   positions 7-8  boss theme    (loops on itself)
#   position  9    game over     (then carries on into the title theme)
#   python3 tools/make_nova_mod.py [out.mod]
import math, random, struct, sys

random.seed(7)
RATE = 8287  # what C-2 (period 428) plays a sample at

def clip(v):
    return max(-128, min(127, int(round(v))))

# ---- samples -------------------------------------------------------------------

def kick():
    out, phase = [], 0.0
    for i in range(2600):
        t = i / RATE
        f = 45 + 140 * math.exp(-t * 35)
        phase += 2 * math.pi * f / RATE
        out.append(clip(125 * math.sin(phase) * math.exp(-t * 8)))
    return out

def snare():
    out, phase, lp = [], 0.0, 0.0
    for i in range(2600):
        t = i / RATE
        phase += 2 * math.pi * 180 / RATE
        lp += 0.6 * (random.uniform(-1, 1) - lp)
        v = 50 * math.sin(phase) * math.exp(-t * 30) + 85 * lp * math.exp(-t * 11)
        out.append(clip(v))
    return out

def hihat():
    out, prev = [], 0.0
    for i in range(600):
        n = random.uniform(-1, 1)
        out.append(clip(70 * (n - prev) * math.exp(-i / RATE * 45)))
        prev = n
    return out

def crash():  # long noise burst for section starts
    out, prev = [], 0.0
    for i in range(9000):
        n = random.uniform(-1, 1)
        hp = n - 0.7 * prev
        prev = n
        out.append(clip(60 * hp * math.exp(-i / RATE * 2.2)))
    return out

def cycle(fn, n, amp):
    return [clip(amp * fn(i / n)) for i in range(n)]

saw = lambda x: 2 * x - 1
pulse25 = lambda x: 1 if x < 0.25 else -0.6
square = lambda x: 1 if x < 0.5 else -1

def bass():  # a saw with its top harmonics rolled off
    n = 64
    return [clip(95 * sum(math.sin(2 * math.pi * k * i / n) / k * (0.85 ** k) for k in range(1, 12)))
            for i in range(n)]

def pad():  # two saws a cycle apart over the loop: a slow chorus shimmer
    n = 2048
    out = []
    for i in range(n):
        a = sum(math.sin(2 * math.pi * k * 32 * i / n) / k for k in range(1, 7))
        b = sum(math.sin(2 * math.pi * k * 33 * i / n) / k for k in range(1, 7))
        out.append(clip(38 * (a + b)))
    return out

def bell():  # FM-ish bell, 32 samples a cycle like the lead
    out = []
    for i in range(4200):
        t = i / RATE
        ph = 2 * math.pi * i / 32
        v = math.sin(ph + 1.8 * math.exp(-t * 6) * math.sin(3.5 * ph))
        out.append(clip(100 * v * math.exp(-t * 3.2)))
    return out

samples = [  # name, data, volume, loop (start, length) in bytes or None
    ("kick", kick(), 64, None),                       # 1
    ("snare", snare(), 50, None),                     # 2
    ("hihat", hihat(), 36, None),                     # 3
    ("bass", bass(), 52, (0, 64)),                    # 4
    ("lead (pulse)", cycle(pulse25, 32, 75), 38, (0, 32)),  # 5
    ("arp (square)", cycle(square, 32, 60), 22, (0, 32)),   # 6
    ("pad (chorus)", pad(), 34, (0, 2048)),           # 7
    ("bell", bell(), 40, None),                       # 8
    ("crash", crash(), 40, None),                     # 9
    ("NOVA STRIKE by Claude", [], 0, None),
    ("for Pico 2 mini OS", [], 0, None),
]
KICK, SNARE, HAT, BASS, LEAD, ARP, PAD, BELL, CRASH = range(1, 10)

# ---- notes and cells ------------------------------------------------------------

# Periods for notes C-1..B-3 (finetune 0).
PERIODS = [856, 808, 762, 720, 678, 640, 604, 570, 538, 508, 480, 453,
           428, 404, 381, 360, 339, 320, 302, 285, 269, 254, 240, 226,
           214, 202, 190, 180, 170, 160, 151, 143, 135, 127, 120, 113]
NAMES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]

def note(name):  # "A2", "G#3" -> period
    return PERIODS[NAMES.index(name[:-1]) + 12 * (int(name[-1]) - 1)]

def up(name, octaves=1):
    return name[:-1] + str(min(3, int(name[-1]) + octaves))

def cell(period=0, sample=0, effect=0, param=0):
    return bytes([(sample & 0xf0) | (period >> 8), period & 0xff, ((sample & 15) << 4) | effect, param])

EMPTY = cell()

def blank():
    return [[EMPTY] * 4 for _ in range(64)]

def set_fx(rows, r, ch, effect, param):  # adds an effect to whatever is there
    c = bytearray(rows[r][ch])
    c[2] = (c[2] & 0xf0) | effect
    c[3] = param
    rows[r][ch] = bytes(c)

MINOR, MAJOR = 0x37, 0x47
CH_DRUM, CH_BASS, CH_LEAD, CH_ARP = range(4)

def drums(rows, style="drive", fill=False):
    for r in range(64):
        b = r % 16
        if style == "drive":
            if b in (0, 8, 10) or (fill and r >= 56 and r % 2 == 0):
                rows[r][0] = cell(note("C2"), KICK) if not (fill and r >= 56 and r % 4 == 2) else cell(note("C2"), SNARE)
            elif b in (4, 12):
                rows[r][0] = cell(note("C2"), SNARE)
            elif r % 2 == 0:
                rows[r][0] = cell(note("C3"), HAT, 0xC, 0x28 if r % 4 else 0x40)
        elif style == "boss":
            if b in (0, 3, 6, 10):
                rows[r][0] = cell(note("C2"), KICK)
            elif b in (4, 12):
                rows[r][0] = cell(note("C2"), SNARE)
            elif b in (8, 14):
                rows[r][0] = cell(note("C3"), HAT, 0xC, 0x40)
            if fill and r >= 60:
                rows[r][0] = cell(note("C2"), SNARE)
        elif style == "soft":
            if b == 0:
                rows[r][0] = cell(note("C2"), KICK, 0xC, 0x28)
            elif b == 8:
                rows[r][0] = cell(note("C3"), HAT, 0xC, 0x18)

def pump_bass(rows, chords):  # octave-jumping eighths, synthwave style
    for bar, (root, _) in enumerate(chords):
        for k in range(8):
            n = root if k % 2 == 0 else up(root)
            rows[bar * 16 + k * 2][1] = cell(note(n), BASS)

def boss_bass(rows, chords):
    for bar, (root, _) in enumerate(chords):
        for k in range(16):
            if k % 4 != 3:
                rows[bar * 16 + k][1] = cell(note(root if k % 8 else up(root)), BASS, 0xC, 0x40 if k % 4 == 0 else 0x28)

def long_bass(rows, chords):
    for bar, (root, _) in enumerate(chords):
        rows[bar * 16][1] = cell(note(root), BASS, 0xC, 0x30)
        rows[bar * 16 + 10][1] = cell(note(root), BASS, 0xC, 0x20)

def arp(rows, chords, every=4):
    for bar, (root, kind) in enumerate(chords):
        for k in range(0, 16, every):
            rows[bar * 16 + k][3] = cell(note(up(root)), ARP, 0x0, kind)
            for j in range(1, every):
                rows[bar * 16 + k + j][3] = cell(0, 0, 0x0, kind)

def pad_chords(rows, chords, ch=3):
    for bar, (root, kind) in enumerate(chords):
        rows[bar * 16][ch] = cell(note(up(root)), PAD, 0x0, kind)
        for k in range(1, 16):
            rows[bar * 16 + k][ch] = cell(0, 0, 0x0, kind)

def bells(rows, chords, ch=2):
    for bar, (root, kind) in enumerate(chords):
        third = 4 if kind == MAJOR else 3
        i = NAMES.index(root[:-1]) + 12 * (int(root[-1]) - 1) + 12
        tones = [i, i + third, i + 7, i + 12, i + 7, i + third]
        for k, r in enumerate((0, 2, 4, 6, 9, 12)):
            n = min(tones[k], 35)
            rows[bar * 16 + r][ch] = cell(PERIODS[n], BELL, 0xC, 0x30 - 4 * k)

def melody(rows, text, inst=LEAD, vib=True):
    tokens = text.split()
    assert len(tokens) == 64, len(tokens)
    for r, t in enumerate(tokens):
        if t == ".":
            continue
        if t == "^":
            rows[r][2] = cell(0, 0, 0xC, 0)
            continue
        rows[r][2] = cell(note(t), inst)
        # vibrato on notes held for 4 rows or more
        held = 0
        while r + held + 1 < 64 and tokens[r + held + 1] == ".":
            held += 1
        if vib and held >= 4:
            for j in range(r + 3, r + held + 1):
                rows[j][2] = cell(0, 0, 0x4, 0x46)

# ---- the song ---------------------------------------------------------------------

AM, F, C, G, E, DM = ("A1", MINOR), ("F1", MAJOR), ("C2", MAJOR), ("G1", MAJOR), ("E1", MAJOR), ("D2", MINOR)
BB = ("A#1", MAJOR)

MEL_A = ("E3 . . . E3 . D3 . E3 . . . A2 . . . "
         "C3 . . . C3 . D3 . C3 . A2 . . . . . "
         "G3 . . . G3 . E3 . G3 . . . C3 . D3 . "
         "D3 . . . . . . . B2 . . . D3 . . . ")
MEL_B = ("A3 . . . G3 . . . E3 . . . C3 . D3 . "
         "C3 . . . A2 . . . C3 . . . F3 . E3 . "
         "D3 . . . B2 . . . D3 . . . G3 . F3 . "
         "E3 . . . . . . . G#2 . . . B2 . . . ")
MEL_C = ("F3 . . . E3 . D3 . A2 . . . D3 . . . "
         "C3 . . . A2 . C3 . F3 . . . E3 . . . "
         "D3 . . . B2 . D3 . G3 . . . F3 . E3 . "
         "E3 . . . . . . . . . . . ^ . . . ")
MEL_BOSS1 = ("A2 . . A2 . . C3 . . A2 . . E3 . D3 . "
             "A#2 . . A#2 . . D3 . . A#2 . . F3 . E3 . "
             "A2 . . A2 . . C3 . . A2 . . E3 . D3 . "
             "A#2 . . A#2 . . D3 . . F3 . . A#3 . A3 . ")
MEL_BOSS2 = ("F3 . . E3 . . C3 . . A2 . . C3 . E3 . "
             "G#2 . . B2 . . E3 . . G#3 . . B3 . . . "
             "F3 . . E3 . . C3 . . A2 . . C3 . E3 . "
             "E3 . . . . . . . . . . . ^ . . . ")
MEL_OVER = ("E3 . . . . . D3 . . . . . C3 . . . "
            "B2 . . . . . . . C3 . . . B2 . . . "
            "A2 . . . . . . . . . . . . . . . "
            "^ . . . . . . . . . . . . . . . ")

def make():
    pats = []

    # 0, 1: title (speed 8): pad, bells, slow bass
    p = blank(); pad_chords(p, [AM, F, C, G]); bells(p, [AM, F, C, G]); long_bass(p, [AM, F, C, G])
    p[0][0] = cell(note("C2"), CRASH, 0xF, 8)
    pats.append(p)
    p = blank(); pad_chords(p, [AM, F, G, E]); bells(p, [AM, F, G, E]); long_bass(p, [AM, F, G, E]); drums(p, "soft")
    set_fx(p, 63, 0, 0xB, 0)  # back to the start of the title
    pats.append(p)

    # 2: battle intro (speed 6): drums, bass and arp build up
    p = blank(); drums(p, fill=True); pump_bass(p, [AM, F, C, G]); arp(p, [AM, F, C, G], 2)
    p[0][0] = cell(note("C2"), CRASH, 0xF, 6)
    pats.append(p)
    # 3: battle A (the loop comes back here)
    p = blank(); drums(p); pump_bass(p, [AM, F, C, G]); arp(p, [AM, F, C, G]); melody(p, MEL_A)
    p[0][0] = cell(note("C2"), CRASH, 0xF, 6)
    pats.append(p)
    # 4: battle B
    p = blank(); drums(p, fill=True); pump_bass(p, [AM, F, G, E]); arp(p, [AM, F, G, E]); melody(p, MEL_B)
    pats.append(p)
    # 5: battle C (bridge), then back to position 3
    p = blank(); drums(p, fill=True); pump_bass(p, [DM, F, G, E]); arp(p, [DM, F, G, E], 2); melody(p, MEL_C)
    set_fx(p, 63, 1, 0xB, 3)
    pats.append(p)

    # 6, 7: boss (speed 5)
    p = blank(); drums(p, "boss"); boss_bass(p, [AM, BB, AM, BB]); arp(p, [AM, BB, AM, BB], 2); melody(p, MEL_BOSS1, vib=False)
    p[0][0] = cell(note("C2"), CRASH, 0xF, 5)
    pats.append(p)
    p = blank(); drums(p, "boss", fill=True); boss_bass(p, [F, E, F, E]); arp(p, [F, E, F, E], 2); melody(p, MEL_BOSS2, vib=False)
    set_fx(p, 63, 1, 0xB, 7)
    pats.append(p)

    # 8: game over (speed 10), then on into the title
    p = blank(); pad_chords(p, [AM, G, F, AM]); long_bass(p, [AM, G, F, AM]); melody(p, MEL_OVER)
    p[0][0] = cell(0, 0, 0xF, 10)
    for r in range(48, 63):
        set_fx(p, r, 3, 0xA, 0x02)  # fade the pad
    set_fx(p, 63, 1, 0xB, 0)
    pats.append(p)

    # positions: title 0-1, battle 2-6, boss 7-8, game over 9
    orders = [0, 1, 2, 3, 4, 3, 5, 6, 7, 8]
    return pats, orders

def write(path):
    pats, orders = make()
    head = b"NOVA STRIKE".ljust(20, b"\0")
    data = b""
    for i in range(31):
        if i < len(samples):
            name, d, vol, loop = samples[i]
            if len(d) % 2:
                d = d + [0]
            ls, ll = loop if loop else (0, 2)
            head += name.encode().ljust(22, b"\0")[:22] + struct.pack(">HBBHH", len(d) // 2, 0, vol, ls // 2, ll // 2)
            data += bytes(v & 0xff for v in d)
        else:
            head += b"\0" * 22 + struct.pack(">HBBHH", 0, 0, 0, 0, 1)
    head += bytes([len(orders), 127]) + bytes(orders).ljust(128, b"\0") + b"M.K."
    body = b"".join(b"".join(b"".join(r) for r in p) for p in pats)
    open(path, "wb").write(head + body + data)
    print(path, len(head + body + data), "bytes")

write(sys.argv[1] if len(sys.argv) > 1 else "examples/NOVA.MOD")
