# Pico 2 mini OS core API

The core of Pico 2 mini OS: what every program on it runs on. The
applications (BASIC, the editor, mc, the tracker, the radio, the network
clients, the serial terminal, the emulators and the clock generator) are
documented in their source comments and their manuals, not here.

## What is here

- **Start-up:** main.c brings up the clocks, PSRAM, HDMI, USB on core 1, the
  drive and the network, then runs the shell.
- **Screen:** dvi.h scans a 640x480 RGB332 framebuffer out over HDMI (with
  line doubling and the mouse arrow); console.h is the 80x30 text console
  with ANSI escape sequences on top of it; screenshot.h saves the screen.
- **Sound:** hdmi_audio.h sends 48 kHz stereo in the HDMI data islands;
  audio.h mixes beeps, notes, the speaker and one source (a player) on
  core 1; chime.h is the start-up chime.
- **USB:** usb_keyboard.h (keys, held keys, gamepads), mouse.h,
  hid_gamepad.h, usb_storage.h (the flash drive, as FatFs drive 0),
  usb_hub_kick.h and usb_log.h.
- **Shell:** shell.h runs commands and scripts; glob.h has the * and ?
  matching and path helpers its file commands use; image.h decodes the
  pictures `view` shows.
- **Memory:** psram_heap.h (the 8 MB PSRAM heap), app_overlay.h (the SRAM
  area one running program has to itself), stack_switch.h and
  core1_job.h.
- **Network:** net.h with wifi.c (the CYW43 radio, WIFI.CFG), net_conn.c
  (TCP connections over lwIP), tls.c (mbedTLS and the CA certificates) and
  clock.c (the clock from NTP).
- **Serial:** serial_console.h, the debug UART as a second console.

The libraries the OS includes keep their own licences: see
THIRD_PARTY_NOTICES.md.
