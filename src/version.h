/// @file version.h
/// The version numbers of the OS and of each program, in one place.
///
/// Each number is MAJOR.MINOR: raise MINOR for a change people would
/// notice, MAJOR for one that breaks their files or habits. The build (the
/// git commit and the day it was built) is stamped in by CMake, so two
/// boards on the same version can still be told apart.
#pragma once

#define OS_NAME "Pico 2 mini OS"  ///< the OS's name, as the banner shows it
#define OS_VERSION "1.0"          ///< the OS's version

#define VER_EDIT "1.1"       ///< edit: text editor
#define VER_BASIC "1.0"      ///< basic: GW-BASIC style BASIC
#define VER_APPLESOFT "1.0"  ///< applesoft: Applesoft BASIC
#define VER_APPLE2E "1.0"    ///< apple2e: Apple IIe emulator
#define VER_C64 "1.0"        ///< c64: Commodore 64 emulator
#define VER_GENESIS "1.0"    ///< genesis: Sega Genesis emulator
#define VER_MC "1.0"         ///< mc: file manager
#define VER_TRACKER "1.0"    ///< tracker: MOD/XM tracker and player
#define VER_TERM "1.2"       ///< term: serial terminal
#define VER_SI5351 "1.0"     ///< si5351: clock generator
#define VER_FM "1.0"         ///< fm: FM radio
#define VER_SPRITE "1.0"     ///< sprite: sprite editor
#define VER_WAV "1.0"        ///< wav: WAV sound editor
#define VER_HEX "1.0"        ///< hex: hex editor
#define VER_MD "1.0"         ///< md: Markdown viewer
#define VER_FONT "1.0"       ///< font: font viewer and benchmark
#define VER_SDR "1.0"        ///< sdr: RTL-SDR receiver
#define VER_VIEW "1.0"       ///< view: picture viewer
#define VER_SWARM "1.0"      ///< swarm: shooter game
#define VER_LANCER "1.0"     ///< lancer: side-scrolling shooter
#define VER_SAY "1.0"        ///< say: text to speech
#define VER_WIFI "1.0"       ///< wifi: WiFi setup
#define VER_RADIO "1.0"      ///< radio: internet radio
#define VER_WWW "1.0"        ///< www: text web browser
#define VER_IRC "1.0"        ///< irc: IRC chat client
#define VER_WGET "1.1"       ///< wget: downloader
#define VER_TELNET "1.0"     ///< telnet: remote terminal
#define VER_SSH "1.0"        ///< ssh: remote shell
#define VER_SCP "1.0"        ///< scp: copy over SSH
#define VER_MOUNT "1.0"      ///< mount: SMB network shares

/// A program and its version.
typedef struct {
    const char *command;  ///< the command that starts it
    const char *name;     ///< what it is, in a few words
    const char *version;  ///< its version
} app_version_t;

/// The programs, in the order help lists them.
extern const app_version_t app_versions[];
/// How many app_versions holds.
extern const int app_version_count;

/// Finds a program's version.
/// @param command its command (not an alias)
/// @return it, or NULL for a command that is part of the OS
const app_version_t *version_find(const char *command);

/// The build: the git commit (with "+" if it had uncommitted changes) and
/// the day it was built, e.g. "7fbd4df 2026-10-30".
/// @return it
const char *version_build(void);
