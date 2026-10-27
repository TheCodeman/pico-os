#!/usr/bin/env python3
# Types lines on the board's serial console (the Debug Probe UART), write-only
# so a terminal program that has the port open keeps reading it. Each
# argument is a line (Python escapes allowed, e.g. "\x1b[15~" for F5); a
# trailing Enter is added unless the line ends with \x00. "@file" sends each
# line of a file.
#   tools/board_type.py "ls /" "mem"
# GAP (s between characters, default 0.002) and LINEWAIT (s after a line,
# default 0.3) in the environment change the pacing.
import os, sys, time
fd = os.open('/dev/serial/by-id/usb-Raspberry_Pi_Debug_Probe__CMSIS-DAP__E6616407E3611C29-if01',
             os.O_WRONLY | os.O_NOCTTY)
gap = float(os.environ.get('GAP', '0.002'))
wait = float(os.environ.get('LINEWAIT', '0.3'))
lines = []
for a in sys.argv[1:]:
    if a.startswith('@'):
        lines += open(a[1:]).read().split('\n')
    else:
        lines.append(a.encode('latin-1').decode('unicode_escape'))
for line in lines:
    end = not line.endswith('\x00')
    for ch in line.rstrip('\x00').encode('latin-1'):
        os.write(fd, bytes([ch]))
        time.sleep(gap)
    if end:
        os.write(fd, b'\r')
    time.sleep(wait)
