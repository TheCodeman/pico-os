# Pico 2 mini OS Internet Radio

Internet radio on the HDMI sound: MP3 and AAC streams over http and https, with a graphical station browser, a station search, favourites, and playing on in the background.

## The radio

![radio](radio.png)

`radio` opens an internet radio in graphics: a list of stations, what is
playing (the station, the song from the stream, bitrate, the buffer, level
meters, the volume) and buttons, with the mouse or the keyboard. MP3 and AAC
streams over http:// and https:// play, as Icecast and Shoutcast send them
(with the song titles), through redirects and .pls/.m3u playlists. AAC means
ADTS streams (audio/aac, audio/aacp). AAC-LC plays in full; HE-AAC (AAC+,
usually 64 kbit/s or less) plays its AAC core only, at half the sample rate
(22 or 24 kHz), so it sounds duller than on a PC: its SBR part needs about
50 KB more of fast memory than the system has to spare. The codec comes
from the stream's Content-Type, or from its first frames when the type
doesn't say, and shows in the lists and under the song (MP3, AAC, or HE-AAC
for a stream at 24 kHz or less). Ogg, Opus, FLAC, MP4 and HLS streams aren't
supported (a search lists only MP3 and AAC stations).

- **Lists:** Favourites, Stations (a built-in list: SomaFM, Radio Paradise,
  Radio Swiss, 181.FM) and Search: Find (or `/`) searches
  radio-browser.info by name, or `tag:jazz`, `country:DE`, `lang:french`,
  most listened first. URL... (or `U`) plays any address.
- **Keys:** Up/Down/PgUp/PgDn choose, Enter plays, S stops, F (or the star
  at the left of a row) adds or removes a favourite (Del in Favourites),
  +/- or Left/Right the volume, M mutes and unmutes (the stream goes on;
  MUTED shows under the volume, and the Mute button beside the slider does
  the same), `[` `]` turn the bass down and up and `{` `}` the treble
  (2 dB a press, -12 to +12; shown beside the volume), Tab the next list,
  Esc leaves.
- **Bass and treble:** shelves at 150 Hz and 4 kHz on the 48 kHz sound.
  The bass starts at +6 dB, since the speakers in a TV or monitor give
  little; set it to 0 for flat on good speakers. A boost first lowers the
  whole by half of it, so loud music isn't squashed by the limiter.
- **Mouse:** click a row, double-click (or right-click) to play, the wheel
  scrolls, drag the volume, click the tabs and buttons.
- **In the background:** leaving with Esc keeps the radio playing at the
  shell prompt and in other programs that don't use the sound (`radio
  stop` stops it; MUSIC, the tracker and the emulators take the sound over).
  `radio status` says what is playing, `radio last` plays the last station
  again, `radio play URL` plays an address, `radio volume 0-100`, `radio
  bass dB`, `radio treble dB`, `radio mute [on|off]` (alone: toggles). **Ctrl+Alt+M** (or a keyboard's Mute
  key) mutes and unmutes the radio from anywhere, in the shell, the editor,
  BASIC or any program, with a note at the top right for a moment.
- Favourites, the last station, the volume and the bass and treble are
  kept in `/RADIO.INI` (`fav=name|url|kbit/s|country|codec` lines,
  `volume=`, `bass=`, `treble=`; it can be edited).
- How it works: a worker in the network's own interrupt reads the stream
  (decrypting https) into a 256 KB buffer in PSRAM (about 16 s at 128
  kbit/s; playing starts after about 2.5 s), taking the song titles out;
  core 1 decodes it with the Helix MP3 or AAC decoder (RealNetworks, fixed
  point, src/radio/helix and src/radio/aac) and resamples it to 48 kHz, then the bass and treble
  (src/radio/radio_tone.c; `tools/radiotest/tonetest.sh [song.mp3]` checks
  them on a PC).
  The decoder's buffers are in the apps' shared SRAM area while it plays
  (no heap); nothing stays allocated after `radio stop`.
