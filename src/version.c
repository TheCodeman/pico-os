/// @file version.c
/// The programs' versions, and the build stamp CMake writes.
#include "version.h"

#include <string.h>

#include "version_build.h"  // VERSION_GIT and VERSION_DATE, from tools/version_build.cmake

const app_version_t app_versions[] = {
    {"edit", "text editor", VER_EDIT},
    {"basic", "GW-BASIC style BASIC", VER_BASIC},
    {"applesoft", "Applesoft BASIC", VER_APPLESOFT},
    {"term", "serial terminal", VER_TERM},
    {"apple2e", "Apple IIe emulator", VER_APPLE2E},
    {"c64", "Commodore 64 emulator", VER_C64},
    {"genesis", "Sega Genesis emulator", VER_GENESIS},
    {"mc", "file manager", VER_MC},
    {"tracker", "MOD music tracker", VER_TRACKER},
    {"si5351", "Si5351A clock generator", VER_SI5351},
    {"fm", "FM radio (Si4703)", VER_FM},
    {"sprite", "sprite editor", VER_SPRITE},
    {"wav", "WAV sound editor", VER_WAV},
    {"hex", "hex editor", VER_HEX},
    {"md", "Markdown viewer", VER_MD},
    {"font", "font viewer (TTF, OTF, PFN)", VER_FONT},
    {"sdr", "RTL-SDR receiver", VER_SDR},
    {"view", "picture viewer", VER_VIEW},
    {"swarm", "Swarm (game)", VER_SWARM},
    {"lancer", "Lancer (game)", VER_LANCER},
    {"say", "text to speech", VER_SAY},
    {"wifi", "WiFi setup", VER_WIFI},
    {"radio", "internet radio", VER_RADIO},
    {"www", "web browser (text)", VER_WWW},
    {"irc", "IRC chat client", VER_IRC},
    {"wget", "download a file", VER_WGET},
    {"telnet", "remote terminal", VER_TELNET},
    {"ssh", "remote shell (SSH)", VER_SSH},
    {"scp", "copy files over SSH", VER_SCP},
    {"mount", "network share (SMB)", VER_MOUNT},
};

const int app_version_count = sizeof app_versions / sizeof *app_versions;

const app_version_t *version_find(const char *command) {
    for (int i = 0; i < app_version_count; ++i)
        if (strcmp(app_versions[i].command, command) == 0)
            return &app_versions[i];
    return NULL;
}

const char *version_build(void) {
    return VERSION_GIT " " VERSION_DATE;
}
