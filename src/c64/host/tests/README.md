# C64 ROM tests

Each `NAME.keys` is typed at the C64 once it has booted (3 seconds of
emulated time); the text screen afterwards must match `NAME.out`.
`NAME.args`, if there, adds options to the `c64` command.

- `boot`: the power-on screen, `38911 BASIC BYTES FREE` and `READY.`
- `basic`: a direct `PRINT` and a short program with `FOR`/`NEXT`
- `expr`: strings, RND, TI, floating point, colours, and POKEs to the
  screen and colour RAM read back with PEEK

They need the KERNAL (901227-03), BASIC (901226-01) and character
(901225-01) ROMs in `ROMS/` at the top of the tree (or `C64ROMS=` a folder);
without them they are skipped. `run_tests.sh ... --expected` rewrites the
`.out` files from what the emulator shows.

`disk` uses a test disk that `c64test --mkd64` makes (`$D64` in `.args`):
LOAD"$",8 and LIST, LOAD and RUN of a BASIC program, LOAD"name",8,1 of
data, and FILE NOT FOUND, all through the KERNAL LOAD trap.

`switch` runs WRECK from the test disk (BASIC and KERNAL banked out, its own
raster interrupt, sprites on, CIA timer stopped), then switches the C64 off
and on and starts HELLO once BASIC is up, as the board's F9 menu does: HELLO
must come up on a clean machine.

`save` saves a BASIC program onto the test disk (in memory only), loads it
back and runs it, and lists the disk.

`cart` plugs in a test cartridge c64test makes (`$CRT`): 8 KB at $8000 with
the CBM80 signature, which the KERNAL starts and which writes CART OK.

`drive` runs drive 8 as a true 1541 on its own ROM (skipped without it):
LOAD"$" and LIST, LOAD and RUN, and SAVE, NEW, LOAD and RUN, all over the
serial bus with the disk as GCR.

`fastboot` boots with the KERNAL's RAM test skipped (`-F`, as the board does) and
reads the screen after only 10 frames: the same power-on screen, BASIC
bytes free and all.
