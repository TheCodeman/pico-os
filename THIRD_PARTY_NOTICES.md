# Third-party code

Pico 2 mini OS's own code is under the MIT licence (`LICENSE`). The
libraries below are included in this repository, or fetched by the Pico SDK,
and **keep their own licences**. Files from them are kept as they came,
apart from changes marked "pico-os" in the source.

| Library | Where | Licence | Origin |
|---|---|---|---|
| FatFs R0.15 | `src/fatfs` | FatFs licence (BSD-style, 1 clause) | ChaN, elm-chan.org |
| LodePNG 20201017 | `src/image/lodepng.*` | zlib | Lode Vandevenne, via LVGL 8.4 |
| TJpgDec R0.03 | `src/image/tjpgd*` | permissive (ChaN) | ChaN, via LVGL 9.2 |
| TinyUSB host driver files (patched) | `src/usb_patch` | MIT | TinyUSB, from Pico SDK 2.3.1 |
| Helix MP3 decoder | `src/radio/helix` | RPSL 1.0 / RCSL (`LICENSE.txt`) | RealNetworks Helix DNA |
| Helix AAC decoder | `src/radio/aac` | RPSL 1.0 / RCSL (`LICENSE.txt`) | RealNetworks Helix DNA, as libhelix-aac |
| Gwenesis (Genesis core) | `src/genesis/core` | **AGPL-3.0** (`LICENSE`) | bzhxx/gwenesis via ducalex/retro-go |
| ... Musashi 68000 (in Gwenesis) | `src/genesis/core/cpus/M68K` | **non-commercial use only** | Karl Stenerud, modified by Eke-Eke |
| ... Z80 (in Gwenesis) | `src/genesis/core/cpus/Z80` | **no commercial distribution** | Marat Fayzullin |
| ... YM2612 (in Gwenesis) | `src/genesis/core/sound/ym2612.c` | **MAME licence (non-commercial)** | MAME fm.c, Genesis Plus GX |
| Font sheets (PNG) | `bfonts`, `examples/FONTS` | unknown: font sheets of unknown origin, supplied by Ken Segler | |
| Mozilla CA certificates | `examples/CACERT.PEM` | MPL 2.0 | curl's cacert.pem |
| Pico SDK 2.3.1 | fetched | BSD-3-Clause | Raspberry Pi |
| lwIP | Pico SDK | BSD-3-Clause | |
| mbedTLS | Pico SDK | Apache-2.0 | |
| TinyUSB | Pico SDK | MIT | |
| cyw43-driver (WiFi) | Pico SDK | Raspberry Pi licence: only with Raspberry Pi parts | |

The RTL-SDR receiver's dongle driver (`src/sdr/rtl2832.c`) is our own
code, written for this project; the register settings it uses for the
RTL2832U and the R820T/R828D tuner (and the tuner's gain steps) follow
those of the open-source rtl-sdr driver (osmocom librtlsdr and the
RTL-SDR Blog fork, GPL-2.0), which documents how these chips are set up.
No code of theirs is included.

## Licence conflicts to be aware of

- The **Genesis emulator core is AGPL-3.0**, so a firmware that includes it
  can only be distributed as a whole under the AGPL. Its 68000, Z80 and
  YM2612 parts carry **non-commercial** terms, which the AGPL itself doesn't
  allow to be added, and which aren't open-source licences.
- The **Helix decoders are RPSL**, a file-level copyleft that sits fine
  next to MIT code, but that the FSF considers incompatible with the GPL
  family, so it conflicts with the AGPL Genesis core in one firmware.
- Everything else is permissive and compatible with MIT.

Publishing the source this way (our code MIT, each library under its own
licence) is fine; the conflicts are about distributing **built firmware**
that contains both the Genesis core and the Helix decoders. To hand out
binaries, build without the Genesis emulator: `cmake -DPICO_OS_GENESIS=OFF`
(it is ON by default) leaves out all of `src/genesis`, and the `genesis`
command then says it isn't in the build.

ROMs, BIOS files and games for the emulators are not included: use your own
copies (see the Emulators manual).
