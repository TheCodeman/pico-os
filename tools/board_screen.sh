#!/bin/sh
# Saves what the board's screen shows as a PNG, read over SWD without
# stopping the board (then switches halting debug off on both cores).
#   tools/board_screen.sh OUT.png [ELF]
set -e
cd "$(dirname "$0")/.."
OUT=$1
ELF=${2:-build/pico2_os.elf}
OCD=$(ls -d ~/.pico-sdk/openocd/*/ | tail -1)
NM=$(ls ~/.pico-sdk/toolchain/*/bin/arm-none-eabi-nm | tail -1)
FB=$($NM "$ELF" | awk '$3=="dvi_framebuf"{print $1}')
TMP=$(mktemp)
timeout 60 "$OCD/openocd" -s "$OCD/scripts" -f interface/cmsis-dap.cfg -f target/rp2350.cfg \
    -c "adapter speed 10000" -c init -c "dump_image $TMP 0x$FB 307200" \
    -c "targets rp2350.dap.core1; write_memory 0xE000EDF0 32 0xA05F0000" \
    -c "targets rp2350.dap.core0; write_memory 0xE000EDF0 32 0xA05F0000" -c shutdown >/dev/null 2>&1
python3 - "$TMP" "$OUT" <<'P'
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
rm -f "$TMP"
