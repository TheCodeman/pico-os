#!/usr/bin/env python3
# compare.py A.wav B.wav: how alike two renders are, by the loudness (RMS)
# of each channel in 10 ms blocks: correlation, and the ratio of levels.
import sys, wave, array, math
def env(path):
    w = wave.open(path); a = array.array("h", w.readframes(w.getnframes()))
    blk = 480; out = ([], [])
    for i in range(0, len(a) // 2 - blk, blk):
        for ch in (0, 1):
            s = a[i * 2 + ch:(i + blk) * 2 + ch:2]
            out[ch].append(math.sqrt(sum(x * x for x in s) / blk))
    return out
a, b = env(sys.argv[1]), env(sys.argv[2])
for ch in (0, 1):
    x, y = a[ch], b[ch]; n = min(len(x), len(y)); x, y = x[:n], y[:n]
    mx, my = sum(x) / n, sum(y) / n
    cov = sum((p - mx) * (q - my) for p, q in zip(x, y))
    vx = math.sqrt(sum((p - mx) ** 2 for p in x)); vy = math.sqrt(sum((q - my) ** 2 for q in y))
    print("%s: envelope correlation %.3f, level ratio %.2f, %d blocks" % ("LR"[ch], cov / (vx * vy) if vx and vy else 0, mx / my if my else 0, n))
