# Pico 2 mini OS

A small command-line operating system for the Raspberry Pi RP2350 (on a
Pimoroni Pico Plus 2): an 80x30 text console on HDMI with sound, a USB
keyboard, mouse and flash drive, WiFi, and a set of programs to go with it.

![Pico 2 mini OS](docs/radio.png)

## Features

- **Shell** with history, Tab completion, wildcards, scripts and /AUTORUN;
  files on a USB drive (FAT/exFAT) and on Windows/Samba shares over WiFi.
- **Programs:** a text editor, a Midnight Commander style file manager
  (`mc`), a picture viewer, a MOD/XM music tracker, a sprite editor, a
  WAV sound editor, a hex editor, a serial terminal with X/Y/ZMODEM, an internet radio (MP3 and AAC),
  a radio receiver for RTL-SDR dongles (wide and narrow FM, AM, with a waterfall),
  Swarm, an arcade space shooter, Lancer, a side-scrolling shooter, and `say`, text to speech over HDMI.
- **Network:** WiFi, `wget` (HTTP/HTTPS), `telnet` with a BBS directory,
  `ssh` and `scp`, network shares (`mount`), NTP.
- **BASIC:** a GW-BASIC style BASIC with graphics, sprites, font sheets,
  music, files, the serial port, GPIO, I2C and SPI.
- **Emulators:** Applesoft BASIC, an Apple IIe, a Commodore 64 and a Sega
  Genesis / Mega Drive (bring your own ROMs).

## Screenshots

Captured with `screenshot` on the board, or drawn by the PC tests in
`tools/` (the same code, the same 640x480 picture).

| | |
|---|---|
| <img src="docs/help.png" width="400" alt="help"><br>help | <img src="docs/hex_menu.png" width="400" alt="Hex editor: the menus"><br>Hex editor: the menus |
| <img src="docs/editor.png" width="400" alt="Text editor (edit)"><br>Text editor (`edit`) | <img src="docs/mc.png" width="400" alt="File manager (mc)"><br>File manager (`mc`) |
| <img src="screenshots/tracker.png" width="400" alt="MOD tracker (on the board)"><br>MOD tracker (on the board) | <img src="docs/tracker_xm.png" width="400" alt="XM in the tracker"><br>XM in the tracker |
| <img src="docs/radio.png" width="400" alt="Internet radio (radio)"><br>Internet radio (`radio`) | <img src="screenshots/sdr.png" width="400" alt="SDR receiver (on the board)"><br>SDR receiver (on the board) |
| <img src="docs/sdr_main.png" width="400" alt="SDR receiver (sdr)"><br>SDR receiver (`sdr`) | <img src="docs/fm_main.png" width="400" alt="FM radio (fm)"><br>FM radio (`fm`) |
| <img src="screenshots/Si5351A.png" width="400" alt="Clock generator (on the board)"><br>Clock generator (on the board) | <img src="docs/si5351_main.png" width="400" alt="Clock generator (si5351)"><br>Clock generator (`si5351`) |
| <img src="docs/sprite_main.png" width="400" alt="Sprite editor (sprite)"><br>Sprite editor (`sprite`) | <img src="docs/wav_main.png" width="400" alt="WAV editor (wav)"><br>WAV editor (`wav`) |
| <img src="docs/hex_main.png" width="400" alt="Hex editor (hex)"><br>Hex editor (`hex`) | <img src="docs/hex_sums.png" width="400" alt="Hex editor: checksums and CRCs"><br>Hex editor: checksums and CRCs |
| <img src="docs/wav_effects.png" width="400" alt="WAV editor effects"><br>WAV editor effects | <img src="docs/swarm_title.png" width="400" alt="Swarm (swarm)"><br>Swarm (`swarm`) |
| <img src="docs/swarm_beam.png" width="400" alt="Swarm: the tractor beam"><br>Swarm: the tractor beam | <img src="docs/swarm_dual.png" width="400" alt="Swarm: the twin fighter"><br>Swarm: the twin fighter |
| <img src="docs/swarm_challenge.png" width="400" alt="Swarm: a challenging stage"><br>Swarm: a challenging stage | <img src="docs/term.png" width="400" alt="Serial terminal (term)"><br>Serial terminal (`term`) |
| <img src="docs/lancer_title.png" width="400" alt="Lancer (lancer)"><br>Lancer (`lancer`) | <img src="docs/lancer_lance.png" width="400" alt="Lancer: a full lance"><br>Lancer: a full lance |
| <img src="docs/lancer_blue.png" width="400" alt="Lancer: the blue pod in the Iron Citadel"><br>Lancer: the blue pod in the Iron Citadel | <img src="docs/lancer_beam.png" width="400" alt="Lancer: the Bastion fires its laser"><br>Lancer: the Bastion fires its laser |
| <img src="docs/telnet.png" width="400" alt="telnet and its BBS list"><br>telnet and its BBS list | <img src="docs/basic_nova.png" width="400" alt="BASIC: the NOVA demo"><br>BASIC: the NOVA demo |
| <img src="docs/basic_house.png" width="400" alt="BASIC graphics"><br>BASIC graphics | <img src="docs/basic_lissa.png" width="400" alt="BASIC: Lissajous figures"><br>BASIC: Lissajous figures |
| <img src="docs/basic_fontdemo.png" width="400" alt="BASIC font sheets"><br>BASIC font sheets | <img src="docs/basic_chart.png" width="400" alt="BASIC: a chart"><br>BASIC: a chart |
| <img src="screenshots/APPLE2E.PNG" width="400" alt="Apple IIe (on the board)"><br>Apple IIe (on the board) | <img src="screenshots/c64.png" width="400" alt="Commodore 64 (on the board)"><br>Commodore 64 (on the board) |
| <img src="screenshots/genesis_launcher.png" width="400" alt="Genesis game launcher (on the board)"><br>Genesis game launcher (on the board) | <img src="screenshots/clock.png" width="400" alt="Processor clock (clock, on the board)"><br>Processor clock (`clock`, on the board) |

## Manuals

PDFs in [manuals/](manuals/), one for the OS and one for each application
(BASIC, the editor, mc, the tracker, the radio, the clock generator, the
FM radio, the sprite editor, the WAV editor, the hex editor, the SDR receiver, Swarm, Lancer, the network tools, the serial terminal, the emulators). They are made from
[docs/](docs/) with `tools/make_manual.py`. The code's API documentation is
made with `doxygen` (the `Doxyfile` here; it goes to `docs/api/html`, and
the docs workflow builds it on each push; it can publish it to GitHub
Pages too, once Pages is set up and the PAGES_ENABLED variable is "true").

## Hardware

- Pimoroni Pico Plus 2 (RP2350B, 16 MB flash, 8 MB PSRAM)
- Pimoroni Pico DVI Sock for HDMI (GPIO 12-19)
- Pimoroni RM2 WiFi breakout (CYW43439)
- A USB hub with a keyboard, a mouse and a flash drive, on the Pico's USB-C
  through an OTG adapter (the hub needs 5 V: see Powering USB devices, under Wiring)
- A Raspberry Pi Debug Probe for flashing and the serial console (GP0/GP1)

## Quick start

With the Pico SDK 2.3.1 and the Arm GNU toolchain (the VS Code "Raspberry
Pi Pico" extension installs both):

```sh
export PICO_SDK_PATH=/path/to/pico-sdk
cmake -S . -B build -G Ninja
cmake --build build
tools/flash.sh                 # with the Debug Probe, or copy build/pico2_os.uf2 in BOOTSEL mode
```

`-DPICO_OS_GENESIS=OFF` builds without the Genesis emulator, whose core
has licence terms that conflict with distributing built firmware (see
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)); it is on by default.

Put the files from `examples/` on the USB drive (as `/EXAMPLES`) to try the
BASIC examples and pictures. No songs are included: `tools/make_demo_mod.py`
and `tools/make_nova_mod.py` make the two the examples use. The details of building and flashing are below.

## Licence

Pico 2 mini OS's own code is under the MIT licence ([LICENSE](LICENSE)).
The libraries it includes keep their own licences: see
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md), which also notes the
licence conflicts to know about before distributing built firmware. No
ROMs or games are included.

## Design notes: how it was built

1. **HDMI text console with sound** (done). 640x480 HDMI from the HSTX
   peripheral, 80x30 text with an 8x16 IBM VGA-style font, output mirrored to the
   Debug Probe UART, and 48 kHz stereo audio carried in the HDMI signal.
2. **USB keyboard through the hub** (done). TinyUSB host with hub and
   HID keyboard support on core 1: US layout, Shift, Caps Lock with its LED,
   Ctrl combinations, key repeat, and ANSI escape sequences for arrow and
   editing keys.
3. **Flash drive and filesystem** (done). TinyUSB mass storage on
   core 1 and FatFs R0.15 on core 0 (FAT12/16/32 and exFAT, long file
   names). Plugging in a drive shows its type, size, free space and the
   files in its top folder. Bulk transfers run on the
   controller's EPX endpoint (`src/usb_patch/hcd_rp2040.c`) rather than
   TinyUSB's polled "interrupt" endpoints, which manage one 64-byte packet a
   millisecond: the drive reads at about 750 KB/s instead of 65 KB/s.
4. **Command shell** (this step). A line editor with history (Up/Down),
   Left/Right, Home/End, Delete, Backspace, Ctrl-C (cancel line) and
   Ctrl-L (clear screen). Lines can be up to 255 characters and wrap over
   several screen rows. Tab completes the first word as a command and later
   words as file and folder names on the drive (also inside a path such as
   `EXAMPLES/IM`; case doesn't matter, the name's own case is used; a name
   with spaces gets a quote). One match is filled in with "/" after a folder
   or a space after anything else; several are filled in as far as they
   agree, and a second Tab lists them. Commands: help, ls/dir, cd, pwd, mkdir, rm/del,
   mv/ren, cat/type, cp/copy, df, mem, memtest, cls/clear, beep [hz], echo,
   reset/reboot. Arguments with spaces can be quoted. Long listings pause
   with "more". `reset` restarts the board after leaving WiFi and
   unmounting the drive; `reset bootsel` starts the USB bootloader instead
   (only useful with the USB port wired to a PC, not the hub). Ctrl-Alt-Del
   on the USB keyboard restarts at once, like a reset button, without
   tidying up first.
5. **VT100 console.** The video console understands the common VT100/ANSI
   escape sequences: cursor movement and positioning, erase, insert and
   delete of characters and lines, scroll regions, 16 colours with bold and
   reverse, save/restore cursor, cursor show/hide and VT100-style line
   wrapping. `colors` in the shell shows the palette.
6. **Text editor.** `edit [file]` (or `ed`) is a full-screen editor with
   nano-style keys: arrows, Home/End, PgUp/PgDn, Backspace/Delete, Tab,
   Enter (keeps the indentation), Ctrl-S save, Ctrl-Q quit (asks to save
   changes), Ctrl-F find (F3 finds the next match, wrapping round), Ctrl-G
   go to line, Ctrl-K cut line (repeat to cut more), Ctrl-U paste, Ctrl-L
   redraw. Files up to 64 KB, kept in SRAM. CR LF or LF line ends are kept
   as they were; a new file is created on first save. Saving writes
   `~edit.tmp` in the same folder and then replaces the original, so a
   failed save never loses it. It draws with VT100 sequences, so it works
   on the serial terminal too (turn off XON/XOFF flow control there, or
   Ctrl-S and Ctrl-Q won't get through).

## BASIC

The full reference is the user's manual, [docs/BASIC_MANUAL.md](docs/BASIC_MANUAL.md).

To keep plain variables fast, `statement()` and `builtin()` in basic.c skip
their keyword chains when the text can't start any keyword in
`statement_words` / `builtin_words`. After adding a statement or function,
run `python3 tools/check_keywords.py`: it checks the lists still cover
every keyword those functions try.

`basic [file]` starts a GW-BASIC style interpreter (it loads and runs `file`
if one is given); `SYSTEM` or `BYE` returns to the shell. Type numbered
lines to build a program, a line number on its own to delete one, and
direct commands at the `Ok` prompt.

- Statements: PRINT (or ?, with ; , TAB() SPC()), INPUT, LINE INPUT, LET,
  IF ... THEN ... ELSE, GOTO, GOSUB/RETURN, FOR/NEXT/STEP, WHILE/WEND,
  ON ... GOTO/GOSUB, END, STOP, CONT, REM and ', DIM, DATA/READ/RESTORE,
  DEF FN, RANDOMIZE, SWAP, CLS, LOCATE, COLOR (PC colours 0-15), BEEP,
  SOUND, TRON/TROFF, several statements per line with `:`; PRINT USING,
  ON ERROR GOTO/RESUME/ERROR, PLAY (music, foreground or background), event
  trapping (ON KEY/TIMER/PLAY/COM GOSUB), MID$ =, OPTION BASE, ERASE,
  DEFINT/DEFSNG/DEFDBL/DEFSTR, DATE$/TIME$, CHDIR/MKDIR/RMDIR, KEY (soft
  keys), ENVIRON, SHELL, and PEEK/POKE/BSAVE/BLOAD on an emulated text
  screen and SCREEN 13.
- Functions: ABS INT FIX SGN SQR SIN COS TAN ATN EXP LOG RND CINT, LEN
  LEFT$ RIGHT$ MID$ CHR$ ASC STR$ VAL INSTR STRING$ SPACE$ HEX$ OCT$ UCASE$
  LCASE$, INKEY$, TIMER, FRE(0), POS(0), CSRLIN, ERR, ERL, DATE$, TIME$,
  ENVIRON$, SCREEN(), PEEK, PMAP, PLAY(), KEYDOWN() (keys held on the USB
  keyboard, an extension); operators + - * / \ ^ MOD,
  comparisons, AND OR XOR NOT EQV IMP.
- Commands: RUN [line or "file"], LIST [range] (pauses every screen), NEW,
  CLEAR, LOAD "file"[,R], SAVE "file", FILES [pattern], KILL, DELETE range,
  RENUM [new][,[old][,step]], AUTO [start][,step], EDIT (opens the program in
  the full-screen editor and reloads it when you quit), EDIT n (edits one
  line), MERGE, CHAIN/COMMON. The input line has cursor-key editing and
  history (Up/Down at the Ok prompt).
- Ctrl-C stops a running program ("Break in 120"); CONT carries on.
- GW-BASIC graphics: SCREEN 1, 2, 7-13 with screen pages, PSET, LINE,
  CIRCLE, PAINT, DRAW, VIEW, WINDOW, GET/PUT, PCOPY (see the manual), and
  sprites (an extension): `SPRITE SHAPE`, `SPRITE n, x, y`, `SPRITE UPDATE`,
  `SPRITEHIT()`: up to 64 see-through sprites that keep what is behind them.
- Font sheets (an extension): `FONT LOAD n, "sheet.png"` reads a picture of
  characters (cells 8x8 to 32x32, sized automatically), `GPRINT x, y, text`
  draws with it at any place, scale and colour, with proportional spacing if
  wanted; `GWIDTH()`, `FONTW()`, `FONTH()`. examples/FONTDEMO.BAS.
- Numbers are single (7 digits), double or 16-bit integer, as in GW-BASIC; strings up to 255 characters of any bytes; names up to 40
  characters; arrays default to 0-10 and may have up to 8 dimensions.
- The program, its variables, strings and arrays get about 8 MB: big blocks
  (arrays) go in the PSRAM and small ones (strings, lines, variables) in SRAM
  while there is room (the start-up banner and FRE(0) show the total). A
  30-minute soak of the whole PSRAM with nothing refreshing it found no
  errors (`memtest soak [minutes] [scrub seconds]` repeats that test in the
  background). A program line is refused
  if it would leave less than 16 KB for the program's data. LOAD reads a
  line at a time, so it needs no extra space.
- LOAD reads plain-text programs and GW-BASIC tokenised files; SAVE always
  writes plain text (`.BAS` is added if there is no extension), so programs
  can be edited in `edit` too.
- Graphics:
  - SCREEN 12 is 640x480 with 16 colours, SCREEN 13 is 320x200 with 256
    colours (an approximation of the VGA palette), and SCREEN 1 (320x200, 4
    colours) and SCREEN 2 (640x200, 2 colours) are for old GW-BASIC
    programs. SCREEN 0 is text. The low-resolution modes are scaled up and
    centred. Text from PRINT and LOCATE stays 80 columns and overlays the
    graphics.
  - PSET, PRESET, LINE (with B, BF and a line style), CIRCLE (arcs, pie
    slices with negative angles, aspect), PAINT (up to one border colour,
    as in GW-BASIC), DRAW (U D L R E F G H M B N C S A TA P, and
    "=variable;"), PALETTE, and COLOR in graphics modes. STEP gives
    relative coordinates.
  - POINT(x,y) returns a pixel's colour number.
  - RGB(r,g,b) is an extension: it returns 256 plus an RGB332 colour, which
    every drawing statement accepts as a direct colour.
  - Differences from GW-BASIC: PALETTE changes what later drawing uses, not
    pixels already on screen; there is no WINDOW, VIEW or GET/PUT.
- GPIO (extensions; pins are GP numbers, and `LED` means the board's LED,
  GP25):
  - `PINMODE pin, INPUT | OUTPUT | PULLUP | PULLDOWN` sets a pin up.
  - `PIN pin, level` drives it (making it an output); `PIN(pin)` reads it
    (making an unused pin an input).
  - `PWM pin, freq [, duty]` runs PWM at freq Hz (8 Hz-10 MHz) with duty 0-100
    (default 50); `PWM pin, 0` stops it. The two pins of a PWM slice (GP2n and
    GP2n+1) share one frequency; changing one says so.
  - `SERVO pin, angle` drives a hobby servo, 0-180 degrees (0.5-2.5 ms pulses
    at 50 Hz).
  - `ADC(pin)` reads GP40-46, 0-4095 for 0-3.3 V; `TEMP()` is the chip's
    temperature in C, using the datasheet's formula. It is uncalibrated, so
    within a few degrees; it varies a little between readings. (On the test
    board it reads about 38 C at 126 MHz and 43 C at 378 with everything
    running; the ADC's reference checks out against VSYS.)
  - `PAUSE ms` waits (Ctrl-C still stops the program); `PINS` lists every
    pin and what it is doing.
  - Pins the system uses are refused with what uses them: GP0-1 (debug
    serial), GP12-19 (HDMI), GP32-36 (WiFi breakout), GP47 (PSRAM).
  - RUN, NEW and leaving BASIC put every pin BASIC used back to an input
    and stop PWM. END, an error or Ctrl-C leave pins as they are, so they
    can be looked at or changed in direct mode.
  - PIN, ADC and TEMP followed by "(" are functions, so arrays can't use
    those names.
- Data files: OPEN (FOR INPUT/OUTPUT/APPEND/RANDOM, and the short "I"/"O"/
  "A"/"R" form), CLOSE, PRINT #, WRITE [#], INPUT #, LINE INPUT #, INPUT$,
  EOF, LOF, LOC, FREEFILE, FIELD, LSET/RSET, GET/PUT, NAME, and MKI$/MKS$/
  MKD$ with CVI/CVS/CVD (MKS$/MKD$ in GW-BASIC's Microsoft Binary Format);
  up to 4 files at once, about 0.8 KB each while open. Strings hold any
  byte, CHR$(0) included (each string keeps its length in a 2-byte header).
- Pictures: `IMAGE "file"[, x, y[, w, h[, fit]]]` draws a BMP, PNG or baseline
  JPEG (decoded into PSRAM, dithered to RGB332) in the current SCREEN mode;
  `SAVEIMAGE "file"` saves the screen as a BMP. The shell has `view file` and
  `screenshot [file]`. PNG decoding is LodePNG (zlib licence) and JPEG is
  ChaN's TJpgDec, both in src/image/ with their notices.
- Serial port: `OPEN "COM1:9600,N,8,1" AS #1` opens UART1 on GP4 (TX) and
  GP5 (RX) (3.3 V levels; other pins with TXn/RXn), used with PRINT #,
  INPUT #, LINE INPUT #, INPUT$, LOC, EOF and LOF; received bytes go into a
  4 KB interrupt-driven buffer. See docs/BASIC_MANUAL.md.
- Not included: PRINT USING, ON ERROR, PLAY, PEEK/POKE, MID$ as a
  statement.
- `examples/` has PRIMES.BAS, GUESS.BAS and BOXES.BAS, the graphics
  demos LISSA.BAS, CHART.BAS, HOUSE.BAS and SHAPES.BAS, and the GPIO demos
  BLINK.BAS (flashes the LED), FADE.BAS (PWM on the LED) and ANALOG.BAS
  (ADC inputs and chip temperature), FILES.BAS (text and random-access
  files), SERIAL.BAS (a terminal on COM1) and SLIDES.BAS (a picture slide
  show; the pictures are in examples/IMAGES). NOVA.BAS is a space shooter with
  sprites, sound effects and its own soundtrack, NOVA.MOD (made by
  tools/make_nova_mod.py, not kept in git); its title pictures NOVAPLAN.PNG and NOVAMOON.PNG
  are rendered by tools/make_nova_planet.py.

## Apple IIe

`apple2e [-d folder] [-n] [-m] [-v 0-9] [disk1 [disk2]]` runs an enhanced
Apple IIe on its own ROM. Copy to a folder `APPLE2E` on the drive: the IIe ROM (16 KB,
or a 32 KB file with the $C000-$FFFF image in its second half, such as
APPLE2E.ROM), the Disk II boot ROM (P5, 341-0027, 256 bytes; any name), and
disk images (140 KB .dsk/.do in DOS 3.3 order, .po in ProDOS order). Disks
are looked for as given, then in the folder; without one the IIe starts in
Applesoft.

Keys: arrows, Delete and Esc as on the Apple. Caps Lock goes on while it
runs, as IIe owners kept it (most older programs, Ultima IV among them,
ignore lower case); the key still switches it. Left Alt or Left Win is Open
Apple, Right Alt or Right Win Solid Apple. F9 lists the disk images in the
folder to put one in drive 1 or 2 (Tab switches drive, E ejects; B puts
it in drive 1 and starts the Apple from it, as does Return into an empty
drive 1); a disk taken out is saved first if it changed, and a disk that
can't be loaded says why on the bottom line. F11 switches fast disk (on unless
-n: while a drive's motor is on the machine runs as fast as it can, with
the speaker quiet). F12 is Ctrl-Reset (with Open Apple: reboot; with both:
self test). F10 saves the disks that changed and returns to the shell,
which then shows how much of real time the emulation needed. The bottom
line shows the keys and the drive lights.

A USB joystick or gamepad (plugged into the hub; HID ones, not Xbox-style
XInput pads) is the IIe's joystick: its stick (or D-pad, if that is a hat
switch) gives paddles 0 and 1 and its first three buttons the IIe's buttons
0-2, alongside the Alt and Win keys. `src/hid_gamepad.c` finds the stick and
buttons in the device's HID report descriptor.

A Mockingboard is in slot 4 (-m leaves it out): two 6522 VIAs with their
timers interrupting the 65C02, and two AY-3-8912s (`src/ay38910.c`) made
into sound on core 1, the first on the left and the second on the right,
mixed with the speaker into the HDMI audio. F7 and F8 turn it down and up
(0-9; 6, the default, is about as loud as the speaker; -v sets it at the
start); the bottom line shows the level for two seconds.

`src/apple2e/` is a full enhanced Apple IIe for running the real ROM (not
shipped; put it in `ROMS/`, which git ignores): the Applesoft app's 65C02
(`src/applesoft/as_cpu.c`), 64 KB main and 64 KB aux RAM, the language card,
the MMU and IOU soft switches with their status reads, keyboard, speaker and
game port, a Disk II card for two 140 KB images (.dsk/.do in DOS 3.3 order,
.po in ProDOS order, read and write), and a renderer for every display mode
into the 640x480 RGB332 framebuffer. It is portable C with no Pico SDK calls except
`a2e_pico.c`, the command. While it runs the scanout shows each framebuffer
row twice (`dvi_line_double`), so the picture takes the first 640x240 and
the Apple's 128 KB and disk track buffers use the second half; the CPU core
and bus run from SRAM. On a PC, `make -C src/apple2e/host test` builds
`build-host/apple2e` and boots `ROMS/APPLE2E.ROM` (or `A2ROM=` a 16 KB
$C000-$FFFF image) to the `]` prompt, runs a BASIC program, checks the
memory switches from machine code, passes the ROM's own self test, boots DOS
3.3 and ProDOS disks and saves to one (`src/apple2e/host/tests/README.md`).
`src/apple2e/PICO_FIT.md` is the plan for fitting it on the board.

## C64 (in progress)

`src/c64/` is a PAL Commodore 64 for running the real ROMs, portable C like
`src/apple2e/`: a 6510 with every undocumented opcode and NMOS decimal mode
(`c64_cpu.c`), the PLA's banking and the $00/$01 port, two CIAs (timers, time
of day, the keyboard matrix and joysticks, the VIC bank), a VIC-II drawn a
line at a time (every text and bitmap mode, scrolling, borders, bad lines,
raster interrupts, sprites with priority and collisions; a register written
part way along a line changes it from there on) into 384x272 colour
numbers, and the SID's registers (no sound yet).

`c64 [-d folder] [-j 1|2] [-r] [file.prg | disk.d64 | cart.crt]` runs it on the board: copy
the KERNAL, BASIC and character ROMs (any names with kernal/kernel, basic and
char in them, or their part numbers) to a folder `C64` on the drive. Finding them
means reading through that folder, which with a game collection in it takes
long (2152 entries: 13 s), so the first start writes them to `/C64ROMS.BIN`
in the drive's root and later starts read that (0.7 s to READY. after a
reset, at once after that); `-r` reads the folder again after the ROMs
change. Each start prints how long its parts took. The F9 list is kept the
same way, in `/C64LIST.BIN` (0.8 s to open after a reset instead of 5 s;
F5 reads the folder again, and a SAVE into it adds its file); it holds up
to 4096 entries. Keys typed on the serial console reach the C64 alongside
a USB keyboard (taps, and F9-F12). As with
the IIe, the scanout doubles lines while it runs, and the C64's RAM, colour
RAM and ROMs live in the framebuffer's second half; the picture is the
320x200 screen doubled both ways with 20 lines of border above and below,
drawn a raster line at a time. The 240 lines shown move up or down by
themselves when a game draws above raster line 31 or below 270 (an open
border, a score panel there), and back after two seconds of nothing
there. A game using more than 240 lines (Frogger Arcade: 1-UP at the top,
CREDIT at the bottom) gets all of them, with some lines left out: ones of a
single colour first. F2 in the F9 menu shows the side borders as
well: all 384 pixels across the 640, every three as five (2, 2 and 1
pixels wide), which is a little uneven, so the sharp 2x view is the default.

The USB keyboard is the C64's (`usb_keyboard_held_keys()` gives the keys
held, and they make up the matrix each frame): letters and digits where the
C64 has them, other symbols as the PC key says (Shift-2 is @), Esc RUN/STOP,
Tab CTRL, Ctrl or Win the C= key, F1-F8 the function keys, F12 RESTORE, F11
reset, F9 a menu and F10 back to the shell. Without one, the serial console
types a key at a time. A USB gamepad or the keypad (8 4 6 2, 7 9 1 3, fire
0 or 5) is joystick 2 (`-j 1` or F1 in the menu: port 1), and a second USB
gamepad is the joystick in the other port (`usb_gamepad_read_n`: the driver
keeps two, in the order they were plugged in).

Drive 8 is answered by trapping the KERNAL's LOAD (no 1541 is emulated): a
.d64 image (read only, in PSRAM) or, with none in it, the folder, whose
.prg files LOAD"NAME",8 finds; LOAD"$",8 lists either. F9 lists the
folders, .d64 and .prg files (up to 2048, two columns of 23 a page; PgUp/
PgDn, Home/End, a letter jumps to the next name starting with it). Reading
a folder shows a progress bar with the count so far; the list is kept while
c64 runs, so the menu opens at once after that (F5 reads it again). Return
on a file switches the C64 off and on (RAM back to its power-on pattern,
every chip reset) and starts the program, or the disk's first file, once
BASIC says READY.; Tab only puts a disk in. F11 also switches it off and
on. A file on the command line is started the same way.

Cartridges: `c64_cart.c` reads .crt files (normal 8 KB, 16 KB and Ultimax,
Ocean, System 3/C64GS, Magic Desk, and EasyFlash with its flash read only
and its 256 bytes of RAM) and switches their banks; the PLA's cartridge
modes map ROML and ROMH. Pick one in F9 or give it on the command line: it
stays plugged in through F11 and the menu's power-ons, and F3 in the menu
takes it out. The image is in PSRAM and the banks are read from there.

A true 1541: F4 in the F9 menu (with the 1541 DOS ROM in the folder: 16 KB
with "1541" or "dos" in its name, or 325302-01 and 901229-0x) makes drive 8
a real 1541 running its own ROM on the serial bus (`c64_drive.c`, VIAs in
`c64_via.c`), for programs with their own loaders. The disk in it is GCR in
PSRAM, read back into the .d64 and written to its file each time the motor
stops after a write; the LED shows in the top border. Its state, RAM and
ROM go in the framebuffer's second half too. It runs in step with the C64
and sleeps while the DOS waits with the motor off; while the motor runs
every other frame isn't drawn, as loading takes both CPUs. Off by default:
the KERNAL trap is instant.

SAVE"NAME",8 is trapped the same way: into the .d64 in the drive (a free
directory entry and blocks from the BAM, the file of the same name
replaced, "@0:" allowed; the image is written back to its file at once, to
a .tmp first and then renamed over it) or, with no disk, as NAME.PRG in the
folder. A full disk says ?NOT OUTPUT FILE.

Sound: `c64_sid.c` is a 6581 SID made a sample at a time (three voices with
all four waveforms, ANDed when combined; ring modulation and hard sync;
the ADSR envelopes with the chip's rate table and exponential decay; a
state-variable filter with a 6581-like cutoff curve; the master volume
with the 6581's DC step, so $D418 digis play). Register writes go to core 1
with their times (`audio_sid_write`) and are played 40 ms later, mixed into
the HDMI audio like the Mockingboard's AY chips; the queue is in PSRAM.
Core 0 keeps a second copy of voice 3 for reading back $D41B and $D41C
(games use its noise for random numbers). On exit, c64 prints how much of
core 1 the SID took. Keypad + and - set its volume (0-9, 6 as the chip
comes). On a PC, `c64 ... -w out.wav` writes the sound.

On a PC, `make -C src/c64/host test` builds `build-host/c64` and
`build-host/c64test`: the tests check the CPU against the 65C02 on every
documented opcode, decimal mode against Bruce Clark's description, the
undocumented opcodes, the CIAs, banking, keyboard and video, then boot the
KERNAL, BASIC and character ROMs from `ROMS/` (not shipped; skipped without
them) to `READY.` and run BASIC (`src/c64/host/tests/README.md`).

## Genesis (in progress)

`genesis [-d folder] [rom.bin]` runs a Sega Genesis / Mega Drive cartridge
(.bin/.gen/.md, or .smd), looked for as given and then in `/GENESIS` on the
drive. Without one it shows a game launcher drawn like the tracker's dialogs
(`src/genesis/genesis_launcher.c`): the folder's games and subfolders, folders
first and then by name, with the highlighted game's cartridge header beside
them (its titles, serial, countries, size; only that header is read). Arrows,
PgUp/PgDn, Home/End or a gamepad's D-pad move; Return, Start or button 1 plays;
Backspace or button 2 goes up a folder; F5 reads the folder again; Esc or F10
goes back to the shell. After a game (F10, or F9 in it) the launcher comes back
with the cursor on that game; F9 in a game started from the shell opens the
launcher in its folder. The core is Gwenesis, as retro-go carries it (`src/genesis/core`; its
source and licences, some of them non-commercial, are in
`src/genesis/README.md`). While it runs the scanout shows each framebuffer
pixel two by two (`dvi_pixel_double`), so the 320x240 picture takes a quarter
of the framebuffer; the rest holds VRAM, the 68000's and Z80's RAM, the
YM2612's main tables, the sound ring, and 60 KB of the core's hottest code
(the 68000's run loop and the opcode handlers games use, the Z80, the VDP and
the YM2612), linked to run there (`src/ld/`, `src/genesis/genesis_fast.h`)
and copied in from flash at the start: from flash it missed the XIP cache it
shares with the ROM in PSRAM all the time. Core 0 runs the CPUs and the sound
chips; core 1 draws each line from a copy of the VDP's state as it was
(`core1_job.h`), and plays the sound, resampled to 48 kHz, into the HDMI
audio. The FM chip works out every other sample, unless `-f`: at its full rate
busy games fall behind and the sound stutters. Sonic's attract demo keeps full
speed, about 55 frames a second drawn. `genesis -b n` times n frames from
power-on; F10 prints where the time went. Keys: arrows;
Z, X, C are A, B, C; Return is Start; or a USB gamepad (buttons 1-3 A, B, C,
any of 8-12 Start; a second gamepad is pad 2). F11 resets, F10 goes back and
says how much of real time the emulation took.

On a PC, `make -C src/genesis/host test` builds `build-host/genesis` (runs a
ROM headless, times each part of a frame with `-p`) and tests the core on a
ROM the test assembles itself; `GENROM=game.bin` times a ROM of your own.

## Network

WiFi comes from Pimoroni's RM2 breakout (a CYW43439) on the Pico Plus 2's
SP/CE connector: power GP32, CS GP33, clock GP34, data GP35 (set in
CMakeLists.txt). The radio runs with the Pico SDK's driver and lwIP
(`pico_cyw43_arch_lwip_threadsafe_background`): lwIP runs in a low-priority
interrupt on core 0, so the network keeps going while a command, the editor
or BASIC runs. The HDMI scanout interrupt stays at the highest priority, and
the radio's PIO state machine and DMA channels are claimed at start-up after
the video's, so they never collide.

- `wifi` shows the connection; `wifi scan` lists networks;
  `wifi connect SSID [password]` joins one (quote names with spaces) and
  saves it to `WIFI.CFG` in the drive's root. **The password is stored in
  plain text.** At start-up, once the drive is mounted, the saved network is
  joined automatically and retried every 30 s if lost. `wifi off`/`on`
  disconnects and reconnects; `wifi forget` also deletes `WIFI.CFG`.
- `wget [-k] URL [file]` downloads over HTTP or HTTPS to the drive, with
  progress, following up to 5 redirects (between http and https too). Esc
  or Ctrl-C stops it and deletes the partial file.
- HTTPS is TLS 1.2 (mbedTLS 3.6 from the Pico SDK), with SNI. The server's
  certificate is checked against `CACERT.PEM` in the drive's root: copy
  `examples/CACERT.PEM` (the Mozilla root bundle, as shipped with Linux and
  curl) there, or fetch curl's copy on the board with
  `wget -k https://curl.se/ca/cacert.pem CACERT.PEM` (that one download
  trusts the network). Only a small index of the file is kept in RAM; the
  root a server needs is read from the drive during the handshake. A
  failed check says why (expired, wrong name, unknown issuer...);
  `wget -k` skips the check. The handshake runs in the wget command itself,
  not in lwIP's interrupt, and takes about a second.
- `date` shows the clock (UTC). It is set over the network (SNTP,
  pool.ntp.org) once WiFi connects, or by hand with
  `date YYYY-MM-DD HH:MM`. Files written to the drive get real dates once it
  is set. Certificate dates can't be checked until it is.
- `ntp [server]` asks an NTP server (default pool.ntp.org, or any name or IP)
  for the time now, sets the clock, and shows how far off it was and the
  round trip.
- `scp [-P port] [-r] user@host:file local` copies a file from an SSH server
  to the drive, and `scp [-P port] [-r] local user@host:file` copies one to
  it; `-r` copies folders. The board asks for the password (it isn't shown
  or saved); it runs `scp -f`/`scp -t` on the server, the classic scp
  protocol, which OpenSSH servers still accept. The SSH client (`ssh.c`)
  speaks curve25519-sha256 or ecdh-sha2-nistp256, takes ecdsa-sha2-nistp256
  or rsa-sha2-256/512 host keys (not ssh-ed25519), and uses aes128-ctr or
  aes256-ctr with hmac-sha2-256; the crypto is mbedTLS. The first time it
  meets a server it shows the host key's SHA256 fingerprint and asks before
  saving it in `/SSH/KNOWN_HOSTS`; a changed key is refused with a warning
  (delete the server's line to accept the new one). Esc or Ctrl-C stops a
  copy, and a partly downloaded file is deleted. It logs in with a key if
  `/SSH/ID_ECDSA` (ECDSA P-256) or `/SSH/ID_RSA` is on the drive and the
  server takes it, otherwise with a password (or keyboard-interactive). Key
  files must be PEM without a passphrase: make one on a PC with
  `ssh-keygen -t ecdsa -b 256 -m PEM -N '' -f id_ecdsa`, copy `id_ecdsa` to
  the drive as `/SSH/ID_ECDSA`, and add `id_ecdsa.pub` to the server's
  `~/.ssh/authorized_keys`. Anyone with the drive has that key, so give it
  only the access it needs. Ed25519 keys aren't supported.
- `mc` is a Midnight Commander style file manager (src/mc): two panels in
  CP437 line graphics and MC's colours, Full/Brief/Long/Info modes,
  sorting, marking, copy/move/delete with progress and overwrite choices,
  mkdir, a text/hex viewer with search, F4 into the editor, find file,
  compare folders, attributes, a user menu (/MC.MNU), pull-down menus, a
  command line, Ctrl-O shell screen, and settings in /MC.INI. It draws by
  changed cells straight into the console (mc_scr.c), keeps listings in
  PSRAM, and works on shares too.
- `tracker [file.mod]` (or `mod`) is a ProTracker-style MOD music editor in
  graphics (src/mod): 4/6/8-channel and 15-sample songs that save back
  byte for byte, every effect, a pattern editor with blocks, transpose and
  record mode, an order list, a sample editor with a waveform, loop and
  selection, WAV/IFF/raw loading, WAV saving and tools, scopes and level
  meters, its own graphical menus, right-click menus and dialogs
  (trk_ui.c), multi-level undo/redo (trk_undo.c), XM songs with an
  instrument editor (16-bit sample tools, envelopes, note map, .XI files),
  MOD to XM conversion (mod2xm.c), and the mouse. The player
  (mod.c, plain C) mixes 48 kHz with interpolation and Amiga panning on
  core 1; tools/modtest renders it on a PC to compare with libopenmpt.
  `modplay file` plays a MOD or an XM: src/mod/xm.c is a FastTracker 2
  player (up to 32 channels, instruments, envelopes, every effect; it saves
  XMs back byte for byte), mixing in blocks; tools/xmtest renders it on a PC,
  converts MODs and checks .XI files. tools/make_demo_mod.py makes DEMO.MOD,
  an original demo song (songs are not kept in git).
- `/AUTORUN` on the drive runs at start-up a line at a time (comments, @,
  echo off; Esc skips or stops it); `run FILE` runs any script; `wifi wait`
  and a mount that never prompts in a script make `mount //pc/share /NET` at
  boot work with the login saved in /SMB/CREDS. examples/AUTORUN is a sample.
- BASIC plays MOD music in the background: `MUSIC "file"[, loop]`, `MUSIC
  STOP/PAUSE/RESUME/VOL n/POS order,row` and `MUSIC(n)` (playing, position,
  row, pattern), through the tracker's player on core 1. The song's data in
  PSRAM is rewritten a little at a time by a timer (this board's PSRAM has
  had fading cells). examples/MUSIC.BAS is a demo.
- Screenshots (src/screenshot.c): Print Screen or Ctrl-Alt-S anywhere asks
  for a name (the next /SCREENSHOTS/SHOTnnnn.PNG) in a box drawn over the
  screen and saves what was shown, as PNG (a small deflate writer: runs and
  repeats of the row above, fixed codes) or BMP. It runs where the program
  reads its next key, so no file work happens in the USB code.
- A USB mouse (src/mouse.c): boot or report protocol (for the wheel), through
  the hub, plugged in any time, also from a keyboard-and-mouse receiver. An
  event queue for programs, an arrow pointer that keeps the pixels under it,
  and a reversed-cell pointer the console draws on the text screen. mc, the
  tracker and BASIC (MOUSEX, MOUSEY, MOUSEB, MOUSEW, MOUSE ON/OFF) use it.
- `mount //server[:port]/share [/NET] [-u [domain\]user] [-g]` puts an SMB
  share (Windows, Samba) in the file tree; `umount` takes it away. The
  client (src/smb/smb.c) does SMB 2.0.2-3.1.1 with NTLMv2 and signing
  (HMAC-SHA256 / AES-CMAC, SHA-512 pre-auth for 3.1.1), no encryption. It
  appears to every program because the link wraps the FatFs calls
  (`--wrap=f_open,...` in CMakeLists.txt): src/smb/smb_vfs.c sends paths on
  a share to SMB and the rest to FatFs, and keeps the current folder when it
  is on a share. Buffers are in PSRAM.
- `cp`, `mv`, `rm` and `ls` take DOS-style wildcards (`*`, `?`, any case,
  `*.*` matching names without a dot) in the last part of a path, on the
  drive and on shares (src/glob.c). Several sources need a folder target;
  wildcard copies ask before overwriting (`-y` doesn't), wildcard `rm` asks
  "Delete N files?" (`-f` doesn't). `cp -r` copies folders (also between
  the drive and shares) and `rm -r` deletes them, after asking.
- `ssh [-p port] [user@]host [command]` is a terminal session on an SSH
  server, on the same transport as scp (keys in /SSH, password otherwise,
  /SSH/KNOWN_HOSTS). It asks for a pty as an 80x30 xterm; the console skips
  OSC strings, has an alternate screen, application cursor keys, DEC line
  drawing and answers cursor-position queries, so top, htop, less and vi
  work. UTF-8 is shown in code page 437. Enter ~. disconnects (~? lists the
  escapes). src/net/ssh_cmd.c.
- `telnet host [port]` is a VT100/ANSI terminal session (port 23 by default).
  It negotiates the usual telnet options (echo, suppress go-ahead, window
  size 80x30, terminal type ANSI then VT100). Ctrl-] quits.
- `term [baud] [format]` (or `serial`) is a serial terminal on COM1 (UART1,
  GP4 TX / GP5 RX by default, the port BASIC calls COM1). Rows 1-29 show
  the far end through the VT100 console; row 30 is a status line. F1 opens
  a settings page (baud 50-4000000, 5-8 data bits, parity, stop bits, pin
  pair, RTS/CTS flow control, local echo, what Enter sends, CR handling,
  Backspace code), saved to /TERM.CFG; F10 exits. Ctrl-A B/C/E/L/F/H send a
  BREAK, clear, toggle echo, log to a file, send a file, and show the keys.
  Ctrl-A U / D upload and download files with XMODEM (checksum, CRC, 1K),
  YMODEM batch or ZMODEM (CRC-16/32, batch, resume); `sz` at the far end
  starts a ZMODEM download by itself. The protocols are in src/xfer, and
  tools/xfertest runs them on a PC against lrzsz. docs/TERM_MANUAL.md has the
  details.

RAM: lwIP and the radio driver take about 40 KB of SRAM (16 packet buffers,
an 8 KB lwIP heap, the driver state), and a connection's 16 KB receive buffer
is allocated while wget or telnet runs; an HTTPS download adds about 40 KB
more (TLS record buffers and handshake state) while it runs. The shell runs on a 16 KB stack of
its own (interrupts, lwIP's included, run on it too); BASIC uses that stack.

## Wiring

| Part | Connection |
| --- | --- |
| Pico DVI Sock | GPIO 12-19 (driven by HSTX) |
| Debug Probe "D" (SWD) | the Pico Plus 2 debug connector |
| Debug Probe "U" (UART) | GP0 = TX, GP1 = RX, GND |
| USB hub (step 2 onwards) | Pico Plus 2 USB-C via a USB-C to USB-A OTG adapter |

### Powering USB devices

The Pico doesn't supply 5 V on its USB port, but the hub and keyboard need
it, and most hubs only connect once they see 5 V from the host side. Two ways
to do it:

- Feed 5 V into the Pico's VBUS pin (pin 40). It reaches the USB-C port
  directly, so the hub and keyboard get power and the Pico runs from it too.
- Use a powered hub that back-feeds 5 V up its host cable. Many do, but not
  all.

Low-speed keyboards behind a hub depend on TinyUSB's support for that on the
RP2350. If a keyboard isn't detected through the hub, try it plugged straight
into the OTG adapter.

## Build

Needs the Pico SDK 2.1 or newer (built and tested with 2.3.1) and the Arm GNU toolchain. The VS Code
"Raspberry Pi Pico" extension installs both; or by hand:

```sh
export PICO_SDK_PATH=/path/to/pico-sdk
cmake -S . -B build -G Ninja
cmake --build build
```

The board defaults to `pimoroni_pico_plus2_rp2350`. If your SDK is older and
does not have it, add `-DPICO_BOARD=pico2`.

The system clock is 252 MHz, an overclock: the core voltage goes up to
1.20 V, the HDMI timing is unchanged and the flash runs at 84 MHz. BASIC
runs about twice as fast as at the chip's usual speeds. The `clock` command
changes the clock the board starts at, from a menu or as `clock 126`,
`clock 252` or `clock 378`, and restarts it: 126 MHz (1.10 V, flash at
42 MHz) or 378 MHz, a further overclock on trial (1.30 V, flash at 94.5 MHz,
PSRAM at 47 MHz) that the heaviest Genesis games need. All three divide to
HSTX's 126 MHz. The setting is kept in the flash's last sector
(`src/clock_setting.c`), read before anything else starts; a board that
fails to start at a saved clock twice in a row (a watchdog runs while it
starts) starts at the default instead. `-DOS_CLOCK_MHZ=` sets the default
(252). The title bar shows the clock the chip really runs at, and its
temperature in degrees F (`src/chip_temp.c`: the on-chip sensor by the
datasheet's formula, within a few degrees); the clock menu and
`clock status` show it too.

<img src="screenshots/clock.png" width="480" alt="The clock menu"><br>
The clock menu at 378 MHz: each clock with its core voltage and flash and
PSRAM speeds, which one is running, saved and the default, and the chip's
temperature.

## Flash and run

The RP2350 needs Raspberry Pi's OpenOCD build; distro packages are usually
too old and fail with "Can't find target/rp2350.cfg". The VS Code extension
installs one under `~/.pico-sdk/openocd`:

```sh
OCD=$(ls -d ~/.pico-sdk/openocd/*/ | tail -1)
"$OCD/openocd" -s "$OCD/scripts" -f interface/cmsis-dap.cfg -f target/rp2350.cfg \
    -c "adapter speed 5000" -c "program build/pico2_os.elf verify" \
    -c "write_memory 0x40018008 32 0x01fffff3; log_output /dev/null; catch {write_memory 0x400d8000 32 0x80000000}; shutdown"
```

The last line restarts the board through its watchdog instead of OpenOCD's
`reset`: after OpenOCD's reset, core 1 (USB and audio) is sometimes left
asleep in the boot ROM until the next power cycle.

Without OpenOCD: hold BOOTSEL while plugging the board's USB-C into the PC
and copy `build/pico2_os.uf2` to the drive that appears.

Open the Debug Probe's serial port at 115200 baud (for example
`picocom -b 115200 /dev/ttyACM0`). The screen shows a one-line banner, checks
the PSRAM (about 3 s), then lists the keyboard, the drive (size, used,
free) and WiFi as they appear, with a spinner while it waits. The prompt
comes once there's a keyboard and a drive; a key typed on the serial
console counts as the keyboard, and after 10 s it starts without a drive.
With both plugged in, the prompt is up about 6 s after a reset. A bell-like
arpeggio chime (src/chime.c) plays 2.5 s in. Later, plugging or unplugging a keyboard or drive and
WiFi connecting each show one short line, e.g. `[USB drive: 59.7 GB,
1 MB used, 59.7 GB free]`. The detail (USB devices and hub ports, the
PSRAM test, the WiFi radio and gateway, HDMI restarts) goes only to the
debug UART.

Audio needs a display or TV with speakers (or an HDMI audio extractor). A
monitor without speakers still shows the picture.

## How the HDMI audio works

The HSTX TMDS encoder only handles pixels, so everything else is sent as
pre-encoded RAW 10-bit symbols:

- Every scanline's 96-pixel hsync pulse holds one data island with two
  packets (`src/hdmi_audio.c`). The DMA IRQ builds each line's blanking
  command list just before it is needed (`build_line` in `src/dvi.c`).
- Line 0 carries the AVI InfoFrame (VIC 1, RGB), line 1 the Audio InfoFrame,
  and every 32nd line an Audio Clock Regeneration packet (N = 6144,
  CTS = 25200 for 48 kHz at 25.2 MHz).
- Audio sample packets hold 4 stereo frames each, and one is sent whenever
  48 kHz worth of credit has built up, which works out to 12,000 a second.
- Core 1 generates the samples (`src/audio.c`) and does the TERC4 and BCH
  encoding, so the IRQ on core 0 only copies 32-word packets. Core 1 also
  runs the USB host stack, so USB interrupts never delay the scanout.

## Notes

- **SRAM rule: an app holds no SRAM while it isn't running.** The heap left
  over is all the system has, so an app allocates its buffers when it starts
  and frees them when it ends (PSRAM for anything big that isn't hot). Code
  or variables that must be in SRAM for speed go in the app's section of the
  shared app area with `APP_OV_FUNC(app, f)` / `APP_OV_DATA(app)`
  (`src/app_overlay.h`, linked as an OVERLAY in
  `src/ld/section_extra_post_data.incl`): `app_overlay_load()` copies the
  app's in when its command starts. The C64, Apple IIe/Applesoft and
  Genesis use it; the radio borrows it for its MP3 or AAC decoder while playing.
  Only the system's own parts (HDMI, audio, USB, network, console, the MOD
  mixer behind BASIC's MUSIC) stay resident. Check with
  `arm-none-eabi-nm --size-sort` on build/pico2_os.elf before adding statics.

- If the hub stops answering a status request (it happens now and then) or
  the drive stops answering a transfer, the board restarts its USB host stack
  ("[USB: ... restarting USB]") and sets every device up again, instead of
  missing plug events until a power cycle. A drive request in progress fails.

- The system clock runs at 126 MHz so the HSTX clock gives the exact
  25.2 MHz pixel clock for 640x480 at 60 Hz.
- The 640x480 RGB332 framebuffer uses 300 KB of the 520 KB SRAM.
- The 8 MB PSRAM is detected at boot by the SDK's `hardware_psram` (it reads
  the chip's ID on GPIO 47), retimed for the 126 MHz clock (42 MHz, chip
  select held at most about 7.6 us; at 63 MHz reads fail while the HDMI
  scanout runs), and tested in full on every boot.
  `psram_malloc()`/`psram_free()` in `src/psram_heap.c` hand it out; `mem`
  shows its use and `memtest [KB]` tests a block and reports the speed. The
  framebuffer, scanout buffers and IRQ code stay in SRAM.
- `src/font8x16.h` is an IBM PC (code page 437) font in the VGA ROM style,
  with the line and block graphics, so DOS ANSI art displays correctly. It
  is generated from the console-setup VGA fonts with
  `tools/gen_font.py --cp437 /usr/share/consolefonts/FullGreek-VGA16.psf.gz
  /usr/share/consolefonts/Uni2-VGA16.psf.gz > src/font8x16.h` (the first has
  all the box-drawing glyphs, the second fills in four accented letters).
  Without `--cp437` the tool makes a Latin-1 font instead.
- `src/fatfs/` is FatFs R0.15 by ChaN (BSD-style licence), copied from the
  Pico SDK's TinyUSB, with our own `ffconf.h`. `src/usb_storage.c` provides
  its disk functions by passing each sector request to core 1, where
  TinyUSB runs.
- After a reset of the Pico alone (the hub keeps its power), this hub's
  status-change interrupt never reports anything, so TinyUSB would neither
  set up the devices still plugged in nor notice plugging and unplugging.
  `src/usb_hub_kick.c` reads every hub port's status each second while USB
  is idle, reports unplugged devices to TinyUSB, and hands it any device
  nobody has set up (clear the change, reset the port, post an attach
  event). It stays off the control pipe while a device is being set up or
  the drive is transferring: on the RP2350 host, control and bulk transfers
  share one endpoint and TinyUSB panics if they overlap.
- `src/dvi.c` is based on `hstx/dvi_out_hstx_encoder` from pico-examples
  (BSD-3-Clause).
