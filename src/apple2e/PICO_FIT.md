# Fitting the Apple IIe on the board

(Done as described under "Plan" below, and further: see `a2e_pico.c`. Once
the C64 emulator's SRAM code arrived the heap at boot was down to about
42 KB, too little for the machine's 21 KB struct as well as the shell, so
the scanout now shows only the 204 rows in use (`dvi_double_top`,
`dvi_double_rows`) and the struct lives in the framebuffer with the RAM and
the disk card. The command takes less than 1 KB of heap.)

Notes for running `src/apple2e` as an OS command, from the current
firmware's map (`build/pico2_os.elf`, 252 MHz build).

## SRAM

The RP2350 has 512 KB of main SRAM (plus two 4 KB banks, which hold the
core stacks). The firmware already uses most of it statically:

| | KB |
| --- | --- |
| HDMI framebuffer, 640x480 RGB332 (`dvi_framebuf`) | 300 |
| everything else in .data/.bss (lwIP pools and heap, USB, line buffers...) | 147 |
| left for the heap at boot | ~77 |

The heap also has to hold the shell's 16 KB stack, and a network
connection's buffers while one is open.

The emulator wants 136 KB of fast memory: 64 KB main, 64 KB aux and 8 KB for
the two language card bank 1s. The CPU touches it on every instruction. Two
140 KB disk images and the 16 KB ROM can live anywhere: the disks are only
read when the head moves to a new track, and each drive's 6.5 KB track
buffer sits in the `a2e_disk2` struct.

**That doesn't fit next to a 300 KB framebuffer.**

## Plan: a line-doubled framebuffer while the emulator runs

The IIe picture is 192 lines, shown doubled. If the scanout repeats each
framebuffer row twice, the picture needs only 640x240 (150 KB). The second
half of `dvi_framebuf` (150 KB) is then free for the emulator's RAM while it
runs, with no allocation at all:

- `src/dvi.c` `build_line()` takes row `(line - FIRST_ACTIVE_LINE)` from the
  framebuffer. A flag that makes it `>> 1` gives line doubling, a one-line
  change in IRQ code that is already in SRAM.
- On entry: set the flag, point `m->main`, `m->aux`, `m->lc1_*` into
  `dvi_framebuf + 640 * 240` (136 KB of the 150 KB), and draw at 560x192
  centred in 640x240. On exit: clear the flag, clear the framebuffer and give
  the screen back to the console, as `asp_screen_end()` does.
- `a2e_video_draw` changes to draw one row per Apple line (Y0 24, no
  vertical doubling): half the work too.

This leaves the heap's ~77 KB alone for the drives' structs (14 KB), the
ROM (16 KB, or read it from flash) and the shell.

## If that isn't enough: hot pages in SRAM, the rest in PSRAM

The page tables (`m->rd`/`m->wr`, one pointer per 256 bytes) let any page
live anywhere, so main RAM, the screen pages and zero page/stack could stay
in SRAM with aux and the language card in PSRAM. PSRAM goes through the
16 KB XIP cache it shares with flash code, and a miss costs about 1 us, so
this is the fallback, not the plan. Either way, `as_cpu_step` and the
`a2e.c` bus functions must run from SRAM (`__not_in_flash_func`), or flash
code competes with emulated memory for that cache.

## Speed

On a PC the machine runs about 330 times faster than a real IIe. At 252 MHz
the board has about 247 clocks per emulated cycle; the CPU core with its
function-pointer bus should need well under a quarter of that. Drawing all
560x192 dots 60 times a second is the bigger cost, so the board's renderer
needs the dirty-row tracking `as_video.c` already does: mark text and
hi-res rows on writes to the screen pages (the write path for pages $04-$0B
and $20-$5F), and redraw only those rows each frame.

Core 1 keeps USB, the drive and audio as now. Speaker clicks go to
`audio_speaker_click()` with the cycle count turned into time, as the
Applesoft app does.
