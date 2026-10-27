# Apple IIe machine tests

Each `NAME.keys` is typed at the keyboard once the machine has booted (a new
line is Return); `NAME.args`, if there, adds options for `apple2e`, and
`NAME.setup` is run first (run_tests.sh describes the variables they can
use). The text screen at the end must match `NAME.out`. Tests whose ROM or
disk images are missing are skipped. `make test` runs them all;
`./run_tests.sh EMU ROM --expected` rewrites the .out files (check them by
hand before committing).

- `boot`: power on to the `Apple //e` banner and the `]` prompt.
- `basic`: a short Applesoft program entered, listed and run.
- `col80`: `PR#3` and 80-column output (aux and main text memory).
- `memory`: a machine-code test entered in the monitor. The bytes it leaves
  at $1000-$100A should be 11 22 22 22 8D 55 66 99 77 8D 0D:
  language card bank 1 and bank 2 written after double reads of $C08B and
  $C083 (11, 22); $C080 write-protects (22); a single read of $C083 after
  $C080 doesn't enable writes (22); RAMWRT status at $C014 while on (8D:
  bit 7 over the last key, Return); aux and main $2000 seen through
  80STORE + HIRES + PAGE2 (55, 66); main and aux zero page through ALTZP
  (99, 77); bank 2 status at $C011 (8D) and ROM read status at $C012 (0D).
- `video`: draws hi-res lines, then checks the board's changed-lines
  renderer against the full one at 640x480 and line-doubled 640x240 (-V).
- `mousetext`: prints the 32 MouseText characters in 80 columns (they dump
  as the letters they replace) and checks the renderer draws them alike.
- `mbtone`: a Mockingboard in slot 4 (-M): machine code in the monitor
  sets VIA 1's ports up, resets the first AY-3-8912 and gives channel A
  tone period 254 at full volume; the chip's registers must read FE 00 ...
  3E 0F and the AY core must make 1023000 / (16 * 254) = 252 Hz.
- `mbirq`: VIA 1's Timer 1 running free every 16384 cycles with its
  interrupt on; a handler at $360 counts the IRQs at $1000 while the next
  monitor line is typed (11 of them), then the interrupt is switched off.
- `lowercase` and `capslock`: a program that, like many old ones, only
  takes a capital Y. Typed `n`, `y` reach it as lower case without -C and
  are refused; with -C (Caps Lock down, as apple2e sets it on the board)
  they arrive as N and Y and the Y is taken.
- `selftest`: Open Apple and Solid Apple held through power-on run the ROM's
  built-in diagnostics, which end in `System OK` (about 40 million cycles).

The disk tests need the Disk II boot ROM (P5, 341-0027) next to the IIe ROM
and these images in `DISKS/` (git ignores both folders; A2DISKS= picks
another folder):

- `dos33jan83.dsk`: the DOS 3.3 System Master (January 1983).
- `dos33master.dsk`: a DOS 3.3 disk (the copy from linapple's images, a
  customised System Master with HELLO, MYHELLO, KEYS, KEYS1 and AA).
- `prodos243.po`: ProDOS 2.4.3 (the A2retroNET build), in ProDOS order.
- `prodos242.dsk`: ProDOS 2.4.2, in DOS 3.3 order.

- `dos33`: boots DOS 3.3 from drive 1 and CATALOGs it.
- `dos33sm`: the same with the System Master (CATALOG stops at a full
  screen, waiting for a key).
- `dossave`: SAVEs a program on a copy of the DOS disk, saved back to the
  file (-W); a second boot CATALOGs it and RUNs it.
- `bootspeed`: the System Master must finish booting (Integer BASIC loaded,
  the `]` prompt) within 13 million cycles, about what a real IIe takes. It
  catches DOS mistaking the disk for stopped and waiting for the motor on
  every call, which made boots several times slower.
- `diskswap`: boots one DOS 3.3 disk, swaps the System Master into drive 1
  (-S, as F9 does on the board) and CATALOGs it.
- `diskboot`: starts with drive 1 empty (the IIe waits for a disk), then
  puts the DOS 3.3 disk in and cold-starts (-B, as F9 then B does on the
  board, or Return into an empty drive 1); DOS boots and CATALOGs it.
- `prodos_po`: boots ProDOS from a .po image to the Bitsy Bye menu.
- `prodos_dsk`: boots ProDOS from a .dsk image, starts BASIC.SYSTEM from
  Bitsy Bye with the arrow keys and CATs the disk.
