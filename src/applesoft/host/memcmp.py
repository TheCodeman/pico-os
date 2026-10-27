#!/usr/bin/env python3
# Compares a memory dump with the oracle's, ignoring what only the real
# firmware leaves in text-page memory: the screen holes it keeps variables
# in, and the cursor it stores into the screen while waiting for a key.
import sys
got, exp = open(sys.argv[1], 'rb').read(), open(sys.argv[2], 'rb').read()
start = int(sys.argv[3], 16)
if len(got) != len(exp):
    sys.exit("dump sizes differ")
cursor_seen = False
for i, (g, e) in enumerate(zip(got, exp)):
    if g == e:
        continue
    a = start + i
    if 0x400 <= a < 0xc00 and (a & 0x7f) >= 0x78:
        continue  # screen hole
    if 0x400 <= a < 0x800 and e == 0xff and not cursor_seen:
        cursor_seen = True
        continue
    sys.exit("first difference at $%04X: got $%02X, expected $%02X" % (a, g, e))
