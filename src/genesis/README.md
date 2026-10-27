# Genesis / Mega Drive (in progress)

`core/` is the Gwenesis emulator core by bzhxx, as retro-go carries it:

- Source: https://github.com/ducalex/retro-go, folder
  `gwenesis/components/gwenesis/src`, at commit
  `4ced120669750ca7228fd0414211430c1d923166` (2026-01-19). That folder is a
  snapshot of https://github.com/bzhxx/gwenesis at `168e466` (2022-08-29)
  with retro-go's changes: the ROM is used where it is loaded (a pointer,
  not an 8 MB array), .smd images are de-interleaved, VRAM is a pointer the
  platform sets, and the VDP draws colour numbers (one byte a pixel, an
  index into the 256-entry `CRAM565`) instead of RGB565.
- Left out: `cpus/Z80/Z80.o` (a stray object file), `ConDebug.c` and
  `Debug.c` (the Z80 debugger), and the `_full` 68000 cycle and jump tables
  (only used with `TABLES_FULL`).
- Changed (each marked "pico-os" in the source): the 68000's and Z80's RAM
  (`M68K_RAM`, `ZRAM`), the YM2612's tables (`tl_tab`, `sin_tab`,
  `lfo_pm_table`; `YM2612SetTables`) and the two sound buffers are pointers
  the platform sets instead of static arrays, so the board can put them where
  there is room; `tl_tab` and `sin_tab` are 16-bit (their values fit; the
  sound is the same to the bit); the YM2612 has a half-rate mode
  (`ym2612_half_rate`, `chan_advance`); `GWENESIS_FAST` marks the Z80's and
  YM2612's hot functions for the board's SRAM overlay (empty elsewhere);
  `gwenesis_bus.c` includes `<stdlib.h>`. The core is built with
  `-DRETRO_GO` and `-Wno-incompatible-pointer-types` (GCC 14 makes that an
  error), as retro-go builds it, and `gwenesis_vdp_gfx.c` with its VDP
  registers, VSRAM and sprite table renamed to `genesis.c`'s copies (so it
  can draw a line as it was while the CPUs go on).

## Licences

Not all of it is free software in the usual sense; check before passing
the binary on.

- Gwenesis (bus, VDP, I/O, Z80 glue, save states): GPLv3 in the files'
  headers; the repository's `LICENSE` (copied to `core/LICENSE`) is the
  AGPLv3.
- 68000: Musashi 3.3 by Karl Stenerud, as modified in Genesis Plus GX by
  Eke-Eke and here by bzhxx. Free for non-commercial use, with credit
  (`core/cpus/M68K/readme.txt`).
- Z80: Marat Fayzullin's Z80 (1994-2007). Not to be distributed
  commercially; he asks to be told of changes.
- YM2612: MAME's fm.c by Jarek Burczynski and Tatsuyuki Satoh, with Eke-Eke's
  fixes from Genesis Plus GX (MAME's old non-commercial terms).
- SN76489: Maxim's, as changed by Charles MacDonald for SMS Plus.

`genesis.c`, `genesis.h` and `host/` are this project's. The frame loop in
`genesis.c` follows retro-go's `gwenesis/main/main.c` (GPLv3).

## Layout

- `genesis.h`/`genesis.c`: the machine behind a small interface, portable C
  with no Pico SDK calls (like `src/c64/`): load a ROM, run a frame with a
  callback per drawn line, pads, palette, sound. The core's platform hooks
  (pad polling, save states: not kept yet) are here too.
- `host/`: the PC build. `make -C src/genesis/host test` builds
  `build-host/genesis` (the harness) and `build-host/genesis_test`, runs the
  tests on a ROM the test assembles itself (68000 and Z80 code, VDP set-up,
  VBlank interrupts, the picture's colours, sound samples), then times the
  harness on it. `GENROM=game.bin` also times a ROM of your own.

`build-host/genesis ROM [-n frames] [-o out.ppm] [-w out.wav] [-p] [-S n]`
runs a ROM headless as fast as it can, prints frames a second, and saves the
last frame and the sound. `-p` splits each frame's time between the 68000
(with its bus and the VDP's ports), the Z80, the VDP's line drawing, the
YM2612 and the SN76489. `-S n` presses Start every n frames.

## Notes for the board

- The core reads anything below 8 MB straight from the ROM buffer, and
  swaps the ROM's bytes in place on loading: the ROM must be writable, and
  reading past its end must be harmless (PSRAM is).
- `genesis_pico.c` has the scanout double pixels both ways, so the picture
  is 320x240 bytes, and puts in the rest of the framebuffer: VRAM, the
  68000's and Z80's RAM, the sound buffers and drawing queue
  (`GENESIS_MEM_SIZE`), the YM2612's main tables (15 KB), the sound ring
  (4 KB) and the `genesis_fast` overlay (60 KB of code, linked to run there
  by `src/ld/section_extra_post_data.incl` and copied in at the start; the
  functions are listed in `genesis_fast.h`, the 68000's opcode handlers in
  `genesis_fast_68k.h`, made by `host/hot_handlers.py` from a profile of
  Sonic). The cartridge and the YM2612's LFO table are in PSRAM. The
  68000's opcode jump table (240 KB) and cycle table (60 KB) are const and
  stay in flash. What stays static is about 14 KB of SRAM.
- Core 1 draws the lines (`genesis_render_pending`, queued by
  `genesis_run_frame` with the VDP's state for each), between its USB work.
- Sound is mono at about 53 kHz, YM2612 and SN76489 separately; the board
  mixes them into a ring and core 1 resamples it to 48 kHz for HDMI.
- Shadow/highlight mode is drawn as normal colours (retro-go's
  simplification).
