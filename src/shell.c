/**
 * @file shell.c
 * @brief Implementation of shell.h: command shell: a line editor with history, and file commands for the USB flash drive (FatFs drive 0:).
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */

#include "shell.h"

#include <ctype.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "apple2e.h"
#include "c64_app.h"
#include "genesis_app.h"
#include "applesoft.h"
#include "app_overlay.h"
#include "audio.h"
#include "basic.h"
#include "editor.h"
#include "ff.h"
#include "dvi.h"
#include "image.h"
#include "net.h"
#include "pico/bootrom.h"
#include "pico/stdlib.h"
#include "hardware/watchdog.h"
#include "hardware/xip_cache.h"
#include "psram_heap.h"
#include "radio.h"
#include "serial_console.h"
#include "glob.h"
#include "mc.h"
#include "smb_vfs.h"
#include "term.h"
#include "trk.h"
#include "mouse.h"
#include "screenshot.h"
#include "si5351_pico.h"
#include "usb_storage.h"

#define LINE_MAX      255  ///< a command may wrap over several screen rows
/// Lines of history kept.
#define HISTORY_LEN   16
/// Screen columns.
#define SCREEN_COLS   80
#define PAGE_LINES    28  ///< console rows less two, for "more" paging
/// The longest folder shown in the prompt.
#define PROMPT_MAX    32

/// The USB drive's FatFs volume.
static FATFS fs;
/// The drive is mounted.
static bool mounted;
static bool started;  ///< the prompt is up (start-up has finished)
/// The start-up screen's WiFi line is still STATUS_ROWS_UP rows above the
/// prompt: nothing has been run or printed since.
static bool first_screen;
#define STATUS_ROWS_UP 5  ///< then Mouse, a blank, "Type help...", a blank, the prompt

/// The line being typed.
static char line[LINE_MAX + 1];
/// Characters in line, and the cursor's position.
///@{
static int len, pos;
///@}
/// The prompt, e.g. "/EXAMPLES> ".
static char prompt[PROMPT_MAX + 8];
/// Its length.
static int prompt_len;
/// Where the cursor is on the screen, counted in characters from the
/// prompt's first one (the line wraps every SCREEN_COLS).
static int cur_off;
static bool last_was_tab;  ///< Tab twice running lists the candidates

/// In PSRAM (SRAM is short), each with a checksum here: a line whose
/// PSRAM cells faded is dropped rather than recalled wrong.
static char (*history)[LINE_MAX + 1];
/// Each history line's checksum.
static uint16_t history_sum[HISTORY_LEN];
static int history_count;  ///< entries stored (up to HISTORY_LEN)
static int history_next;   ///< slot the next entry goes in
static int history_view;   ///< 0 = editing a new line, n = n entries back

static int esc_state;  ///< 0 none, 1 after ESC, 2 inside ESC [ or ESC O
/// The parameter byte of the escape sequence being read.
static char esc_param;

// ---- Output helpers -------------------------------------------------------
// The line being typed can wrap over several screen rows, so the editor keeps
// track of the cursor as an offset from the start of the prompt and moves it
// with VT100 sequences; after a change it redraws the prompt and line.

/// Makes the prompt from the current folder.
static void make_prompt(void) {
    char cwd[256] = "";
    if (mounted && f_getcwd(cwd, sizeof cwd) == FR_OK) {
        // Keep the tail of a long path so the prompt stays short.
        int n = strlen(cwd);
        const char *shown = n > PROMPT_MAX - 4 ? cwd + n - (PROMPT_MAX - 7) : cwd;
        snprintf(prompt, sizeof prompt, "%s%.32s> ", shown == cwd ? "" : "...", shown);
    } else {
        snprintf(prompt, sizeof prompt, "> ");
    }
    prompt_len = strlen(prompt);
}

/// Moves the cursor to offset `off` from the start of the prompt.
/// @param off the offset
static void cursor_to(int off) {
    int rows = off / SCREEN_COLS - cur_off / SCREEN_COLS;
    if (rows < 0)
        printf("\x1b[%dA", -rows);
    else if (rows > 0)
        printf("\x1b[%dB", rows);
    printf("\x1b[%dG", off % SCREEN_COLS + 1);
    cur_off = off;
}

/// Redraws the prompt and the line and puts the cursor at pos.
static void refresh(void) {
    cursor_to(0);
    printf("%s%.*s", prompt, len, line);
    int end = prompt_len + len;
    // A line ending exactly at the edge leaves the cursor waiting to wrap;
    // move it on so the screen agrees with `end`.
    if (end % SCREEN_COLS == 0)
        printf("\r\n");
    printf("\x1b[J");  // clear what's left of a longer old line
    cur_off = end;
    cursor_to(prompt_len + pos);
}

/// Starts a new prompt on the current (empty) screen row.
static void new_prompt(void) {
    make_prompt();
    cur_off = 0;
    refresh();
}

/// Replaces the line typed (history).
/// @param text the new line
static void replace_line(const char *text) {
    snprintf(line, sizeof line, "%s", text);
    len = pos = strlen(line);
    refresh();
}

/// Moves the cursor past the end of the line, onto a fresh row.
static void leave_line(void) {
    int end = prompt_len + len;
    cursor_to(end);
    if (end % SCREEN_COLS)
        printf("\r\n");
}

// ---- Commands -------------------------------------------------------------

/// Whether the drive is there (saying so if not).
/// @return it is
static bool need_drive(void) {
    if (!mounted)
        printf("No USB drive. Plug one into the hub.\n");
    return mounted;
}

const char *fresult_name(FRESULT r) {
    static const char *const names[] = {
        "ok", "disk error", "internal error", "drive not ready", "file not found",
        "path not found", "invalid name", "access denied", "already exists",
        "invalid object", "write protected", "invalid drive", "not enabled",
        "no filesystem", "mkfs aborted", "timeout", "locked", "out of memory",
        "too many open files", "invalid parameter"};
    return (unsigned)r < sizeof names / sizeof names[0] ? names[r] : "error";
}

/// Prints a FatFs error.
/// @param what what failed
/// @param r the error
static void print_error(const char *what, FRESULT r) {
    printf("%s: %s\n", what, fresult_name(r));
}

/// Waits for a key while paging; returns false if the user wants to stop.
/// @return go on
static bool more(void) {
    printf("-- more (any key, q to stop) --");
    fflush(stdout);
    int c;
    while ((c = getchar_timeout_us(100000)) == PICO_ERROR_TIMEOUT)
        ;
    printf("\r                                \r");
    return c != 'q' && c != 'Q' && c != 3 && c != 0x1b;
}

// ---- help ------------------------------------------------------------------

/// One entry per command: its name, other names, a line for the overview, and
/// the usage and details `help name` prints.
typedef struct {
    /// The command's name, other names, one-line summary, and usage.
    ///@{
    const char *name, *aliases, *summary, *usage;
    ///@}
} help_t;

/// Help: files.
static const help_t help_files[] = {
    {"ls", "dir", "list a folder",
     "ls [folder]   or   ls [folder/]pattern   e.g. ls EXAMPLES/*.BAS\n"
     "  Lists the files (with sizes) and folders, a screen at a time:\n"
     "  any key for more, q to stop. * and ? match any letters / one letter."},
    {"cd", NULL, "change folder",
     "cd [folder]\n"
     "  Changes the current folder; cd alone goes to /. .. is the parent."},
    {"pwd", NULL, "show the current folder", "pwd"},
    {"mkdir", NULL, "make a folder", "mkdir folder"},
    {"rm", "del", "delete file/empty folder",
     "rm [-r] [-f] path...\n"
     "  Deletes files and empty folders. * and ? work: rm OLD/*.TMP asks\n"
     "  \"Delete N files?\" first. -r deletes folders and everything in\n"
     "  them (it asks first). -f: don't ask. Esc stops."},
    {"cp", "copy", "copy a file",
     "cp [-r] [-y] from to\n"
     "cp [-r] [-y] from... folder\n"
     "  * and ? work in the last part of a name: cp EXAMPLES/*.BAS /NET.\n"
     "  Several files need a folder to copy into; they keep their names.\n"
     "  -r copies folders with all in them (cp -r EXAMPLES /NET/EX).\n"
     "  Copies of several files ask before overwriting (y/N/a), unless -y.\n"
     "  Esc stops."},
    {"mv", "ren", "rename or move",
     "mv from to\n"
     "mv from... folder\n"
     "  Renames, or moves into a folder (on the same drive). * and ? work."},
    {"cat", "type", "show a text file",
     "cat file\n"
     "  Pages like ls. Colour codes in the file show as colours;\n"
     "  Ctrl-Z ends the text."},
    {"df", NULL, "size and free space", "df"},
};

/// Help: screen and sound.
static const help_t help_screen[] = {
    {"cls", "clear", "clear the screen", "cls"},
    {"echo", NULL, "print text", "echo text"},
    {"colors", NULL, "show the 16 colours", "colors"},
    {"beep", NULL, "play a tone",
     "beep [hz]\n"
     "  Plays a 300 ms tone, 20-20000 Hz (default 880), over HDMI audio."},
    {"modplay", NULL, "play a MOD or XM song",
     "modplay file\n"
     "  Plays a .MOD or .XM (FastTracker 2) song once through HDMI audio;\n"
     "  any key stops."},
    {"screenshot", NULL, "save the screen (PNG/BMP)",
     "screenshot [file]\n"
     "  .PNG or .BMP by the name; default the next /SCREENSHOTS/SHOTnnnn.PNG.\n"
     "  Print Screen (or Ctrl-Alt-S) anywhere takes one and asks for the name."},
};

/// Help: programs.
static const help_t help_programs[] = {
    {"edit", "ed", "text editor",
     "edit [file]\n"
     "  Ctrl-S save   Ctrl-Q quit   Ctrl-F find   F3 find next\n"
     "  Ctrl-G go to line   Ctrl-K cut line   Ctrl-U paste   Ctrl-L redraw"},
    {"basic", NULL, "GW-BASIC style BASIC",
     "basic [file]\n"
     "  Starts BASIC, or loads and runs a program. SYSTEM returns here."},
    {"applesoft", NULL, "Applesoft BASIC (Apple II)",
     "applesoft [file]\n"
     "  An Apple IIe with Applesoft BASIC: its text, lo-res and hi-res\n"
     "  screens, and DOS 3.3 style commands (CATALOG, LOAD, SAVE, RUN...)\n"
     "  on the drive. Loads and runs file if given. BYE returns here."},
    {"term", "serial", "serial terminal (COM1)",
     "term [baud] [format]       e.g. term, term 9600, term 9600 7E1\n"
     "  A terminal on COM1 (UART1, GP4 TX / GP5 RX unless changed).\n"
     "  F1 settings (speed, format, pins, flow control, echo, newlines)\n"
     "  F10 exit   Ctrl-A U upload, Ctrl-A D download (XMODEM, YMODEM, ZMODEM;\n"
     "  sz at the far end starts a ZMODEM download by itself).\n"
     "  Ctrl-A H shows the other keys: break, clear, log, send file.\n"
     "  Settings are saved in /TERM.CFG."},
    {"apple2e", NULL, "Apple IIe emulator",
     "apple2e [-d folder] [-n] [-m] [-v 0-9] [disk1 [disk2]]\n"
     "  An enhanced Apple IIe running its own ROM, with a Disk II for\n"
     "  140 KB .dsk/.do/.po images. The ROMs go in /APPLE2E (or -d folder);\n"
     "  disks are looked for as given, then there. F9 changes disks; F10\n"
     "  saves changed disks and returns here; F12 is Ctrl-Reset; Alt/Win\n"
     "  are the Apple keys. The disk runs fast (F11 switches it; -n starts\n"
     "  with it off). A USB joystick or gamepad is the IIe's joystick. A\n"
     "  Mockingboard is in slot 4 (-m leaves it out; F7/F8 or -v set its\n"
     "  volume, 6 at first)."},
    {"c64", NULL, "Commodore 64 emulator",
     "c64 [-d folder] [-j 1|2] [file.prg | disk.d64 | cart.crt]\n"
     "  A PAL C64 running its own ROMs: the KERNAL, BASIC and character\n"
     "  ROMs go in /C64 (or -d folder). Drive 8 is a .d64 or the folder's\n"
     "  .prg files (LOAD and SAVE work on both); a file given is started\n"
     "  once BASIC is up. .crt cartridges: normal, Ultimax, Ocean, System 3,\n"
     "  Magic Desk, EasyFlash (from F9 too; F3 there takes one out).\n"
     "  F4 in F9 makes drive 8 a true 1541 (its ROM in the folder too).\n"
     "  Esc is RUN/STOP, Tab CTRL, Ctrl/Win C=; F12 RESTORE, F11\n"
     "  power off/on, F9 menu (disks, programs, joystick port), F10 back.\n"
     "  Joysticks: USB gamepads (the first in port 2, a second in port 1)\n"
     "  or the keypad (8 4 6 2, fire 0 or 5) with the first;\n"
     "  keypad + and - set the volume."},
    {"genesis", NULL, "Sega Genesis emulator",
     "genesis [-d folder] [-f] [rom.bin]\n"
     "  A Sega Genesis / Mega Drive. Without a ROM it lists the games in\n"
     "  /GENESIS (or -d folder) to pick one (Return or Start plays, Esc\n"
     "  leaves); a ROM is looked for as given and then there. F9 in a game\n"
     "  goes back to the list. -f: full-quality FM sound (busy games may\n"
     "  stutter); -b n: a benchmark of n frames.\n"
     "  Arrows; Z, X, C are A, B, C; Return Start; or a USB gamepad (a\n"
     "  second one is pad 2). F11 resets, F10 goes back."},
    {"mc", NULL, "file manager",
     "mc [left-folder] [right-folder]\n"
     "  Two-panel file manager like Midnight Commander. F1 help, F3 view,\n"
     "  F4 edit, F5 copy, F6 move, F7 mkdir, F8 delete, F9 menus, F10 quit,\n"
     "  Tab other panel, Insert mark, typing runs commands. Esc then 1..0 = F1..F10."},
    {"tracker", "mod", "MOD music tracker",
     "tracker [file.mod]\n"
     "  A ProTracker-style editor for .MOD songs (4, 6 or 8 channels) in\n"
     "  graphics: patterns, order list, samples (WAV/IFF/raw), scopes. F1 keys,\n"
     "  F9 menus, F5 play, F8 stop, Space record. Works with a mouse too."},
    {"si5351", "clockgen", "Si5351A clock generator",
     "si5351                     the panel: three outputs, 2.5 kHz-200 MHz\n"
     "si5351 set N FREQ          e.g. si5351 set 0 10M (k, M or Hz)\n"
     "si5351 on N | off N        an output on or off (off all: every one)\n"
     "si5351 status              the setup, PLLs and the chip's status\n"
     "  Also drive, pll, invert, phase, xtal, load, cal, iq, recall, i2c\n"
     "  (si5351 help). I2C1 on GP2/GP3 at 0x60; kept in /SI5351.INI."},
    {"view", NULL, "show a picture",
     "view file\n"
     "  Shows a BMP, PNG or JPEG; any key returns."},
};

/// Help: network.
static const help_t help_network[] = {
    {"wifi", NULL, "WiFi status and setup",
     "wifi                       status and IP address\n"
     "wifi scan                  list networks\n"
     "wifi connect SSID [pw]     connect and save to WIFI.CFG\n"
     "wifi off | on              radio off / on\n"
     "wifi wait [seconds]        wait until connected (for /AUTORUN)\n"
     "wifi forget                delete WIFI.CFG"},
    {"radio", NULL, "internet radio",
     "radio                      the station browser (MP3 streams)\n"
     "radio play URL | last      play an address, or the last station\n"
     "radio stop | status        stop it, or say what is playing\n"
     "radio volume 0-100         set the volume\n"
     "radio mute [on|off]        silence it (alone: toggles)\n"
     "  Ctrl+Alt+M mutes and unmutes it from anywhere.\n"
     "  Favourites, the last station and the volume are kept in /RADIO.INI."},
    {"wget", NULL, "download a file",
     "wget [-k] URL [file]\n"
     "  http or https. -k skips the certificate check (CACERT.PEM).\n"
     "  The file name defaults to the last part of the URL."},
    {"telnet", NULL, "remote terminal",
     "telnet host [port]\n"
     "telnet [-m [file]]          a directory of BBSes to pick from\n"
     "  Port 23 unless given. Ctrl-] quits.\n"
     "  The directory is /TELNET/DIALDIR.XML (else the share's\n"
     "  /NET/telnet_list/dialdirectory.xml, copied there): EtherTerm\n"
     "  <BBS name ip port protocol> entries. Type to filter, Enter calls\n"
     "  (SSH ones with ssh), F2 marks a favourite (/TELNET/FAVS.TXT),\n"
     "  Tab shows the favourites, Esc leaves."},
    {"ssh", NULL, "remote shell (SSH)",
     "ssh [-p port] [user@]host [command]\n"
     "  A terminal on an SSH server, or one command's output. Asks for\n"
     "  the password, or logs in with /SSH/ID_ECDSA or /SSH/ID_RSA.\n"
     "  Server keys: /SSH/KNOWN_HOSTS. Enter ~. disconnects, ~? lists more."},
    {"scp", NULL, "copy files over SSH",
     "scp [-P port] [-r] user@host:path local\n"
     "scp [-P port] [-r] local user@host:path\n"
     "  -r copies folders. Asks for the password, or logs in with\n"
     "  /SSH/ID_ECDSA or /SSH/ID_RSA. Server keys: /SSH/KNOWN_HOSTS."},
    {"mount", "umount", "network share (SMB)",
     "mount //server[:port]/share [/NET] [-u [domain\\]user] [-g]\n"
     "  Puts a Windows or Samba share at /NET (or the folder given), for every\n"
     "  command and BASIC. Asks for the user and password (-g: guest); can save\n"
     "  them in /SMB/CREDS. mount alone lists the shares; umount [/NET] ends one."},
    {"ntp", NULL, "set the clock (NTP)",
     "ntp [server]\n"
     "  Asks a time server (default pool.ntp.org) and sets the clock."},
};

/// Help: system.
static const help_t help_system[] = {
    {"date", NULL, "show or set the clock",
     "date\n"
     "date YYYY-MM-DD HH:MM[:SS]\n"
     "  The clock is UTC, set from the network once WiFi connects."},
    {"mem", NULL, "memory use", "mem"},
    {"mouse", NULL, "mouse status and speed",
     "mouse [speed 1-10 | accel on|off]\n"
     "  Shows the mouse; sets how fast the pointer moves (5 is the default)\n"
     "  and whether fast moves go further. Saved in /MOUSE.CFG."},
    {"memtest", NULL, "test the PSRAM",
     "memtest [KB]                       quick test (default 1024 KB)\n"
     "memtest soak [minutes] [scrub-s]   long test in the background\n"
     "memtest soak stop"},
    {"reset", "reboot", "restart the board",
     "reset            restart\n"
     "reset bootsel    restart into the USB bootloader (to copy a .uf2)\n"
     "  Ctrl-Alt-Del on the keyboard also restarts."},
    {"run", "source", "run a script",
     "run FILE\n"
     "  Runs each line of FILE as a command (# or REM: comments; @ or echo off:\n"
     "  don't show them; Esc stops it). /AUTORUN runs this way at start-up\n"
     "  (Esc during start-up skips it), e.g.  wifi wait 20  then  mount //pc/share."},
    {"help", NULL, "this list",
     "help [command]\n"
     "  Lists the commands, or shows how to use one."},
};

/// A group of commands in help's list.
typedef struct {
    const char *title;  ///< its title
    const help_t *cmds;  ///< its commands
    int count;  ///< their count
} help_group_t;

/// A help group from a title and a list.
#define HELP_GROUP(title, list) {title, list, sizeof list / sizeof *list}
/// The left column's groups.
static const help_group_t help_left[] = {
    HELP_GROUP("Files", help_files),
    HELP_GROUP("Programs", help_programs),
};
/// The right column's groups.
static const help_group_t help_right[] = {
    HELP_GROUP("Screen and sound", help_screen),
    HELP_GROUP("Network", help_network),
    HELP_GROUP("System", help_system),
};

#define HELP_HEAD "\x1b[96m"  ///< section titles
#define HELP_CMD  "\x1b[97m"  ///< command names
#define HELP_DIM  "\x1b[90m"  ///< the summaries
#define HELP_END  "\x1b[0m"  ///< back to normal

/// Row `row` of a column of groups (a blank line between groups): prints it
/// padded to `width` columns, or unpadded if width is 0. False past the end.
/// @param groups the column's groups
/// @param n their count
/// @param row the row
/// @param width the column's width
/// @return the column has that row
static bool help_row(const help_group_t *groups, int n, int row, int width) {
    for (int g = 0; g < n; ++g) {
        int rows = 1 + groups[g].count;
        if (row < rows) {
            if (row == 0)
                printf(HELP_HEAD "%-*s" HELP_END, width, groups[g].title);
            else
                printf("  " HELP_CMD "%-12s" HELP_END "%-*s", groups[g].cmds[row - 1].name,
                       width ? width - 14 : 0, groups[g].cmds[row - 1].summary);
            return true;
        }
        row -= rows;
        if (row == 0 && g + 1 < n) {
            printf("%*s", width, "");
            return true;
        }
        --row;
    }
    printf("%*s", width, "");
    return false;
}

/// A command's help, by name or alias.
/// @param name the name
/// @return it, or NULL
static const help_t *find_help(const char *name) {
    const help_group_t *sides[] = {help_left, help_right};
    int counts[] = {sizeof help_left / sizeof *help_left, sizeof help_right / sizeof *help_right};
    for (int s = 0; s < 2; ++s)
        for (int g = 0; g < counts[s]; ++g)
            for (int i = 0; i < sides[s][g].count; ++i) {
                const help_t *h = &sides[s][g].cmds[i];
                if (!strcmp(name, h->name) || (h->aliases && !strcmp(name, h->aliases)))
                    return h;
            }
    return NULL;
}

/// help [command]: the list, or one command's usage.
/// @param argc the argument count
/// @param argv the arguments
static void cmd_help(int argc, char **argv) {
    if (argc > 1) {
        const help_t *h = find_help(argv[1]);
        if (!h) {
            printf("help: no command %s (type help for the list)\n", argv[1]);
            return;
        }
        printf(HELP_CMD "%s" HELP_END " - %s", h->name, h->summary);
        if (h->aliases)
            printf(HELP_DIM " (also %s)" HELP_END, h->aliases);
        printf("\n\n%s\n", h->usage);
        return;
    }
    // Two columns, one command per line.
    int nl = sizeof help_left / sizeof *help_left, nr = sizeof help_right / sizeof *help_right;
    for (int row = 0;; ++row) {
        bool more_left = help_row(help_left, nl, row, 40);
        bool more_right = help_row(help_right, nr, row, 0);
        printf("\n");
        if (!more_left && !more_right)
            break;
    }
    printf(HELP_DIM "Type help and a command name for how to use it, e.g. help wifi.\n"
           "Also: dir=ls del=rm copy=cp ren=mv type=cat clear=cls ed=edit reboot=reset\n"
           "Keys: Tab completes, Up/Down history, Ctrl-C cancels, Ctrl-L clears.\n" HELP_END);
}

/// ls [path or pattern].
/// @param argc the argument count
/// @param argv the arguments
static void cmd_ls(int argc, char **argv) {
    if (!need_drive())
        return;
    DIR dir;
    FILINFO info;
    const char *path = argc > 1 ? argv[1] : "", *pattern = NULL;
    char folder[256];
    if (glob_wild(path)) {  // ls EXAMPLES/*.BAS: that folder, matching names only
        const char *slash = strrchr(path, '/');
        pattern = slash ? slash + 1 : path;
        snprintf(folder, sizeof folder, "%.*s", slash ? (int)(slash - path) : 0, path);
        path = slash && !folder[0] ? "/" : folder;
    }
    FRESULT r = f_opendir(&dir, path);
    if (r != FR_OK) {
        print_error(path, r);
        return;
    }
    int files = 0, dirs = 0, lines = 0;
    unsigned long long bytes = 0;
    while ((r = f_readdir(&dir, &info)) == FR_OK && info.fname[0]) {
        if ((info.fattrib & (AM_HID | AM_SYS)) || (pattern && !glob_match(pattern, info.fname)))
            continue;
        if (info.fattrib & AM_DIR) {
            printf("  %-50s      <DIR>\n", info.fname);
            ++dirs;
        } else {
            printf("  %-50s %10llu\n", info.fname, (unsigned long long)info.fsize);
            bytes += info.fsize;
            ++files;
        }
        if (++lines == PAGE_LINES) {
            lines = 0;
            if (!more())
                break;
        }
    }
    f_closedir(&dir);
    printf("  %d file%s, %d folder%s, %llu bytes\n", files, files == 1 ? "" : "s", dirs,
           dirs == 1 ? "" : "s", bytes);
}

/// cd [folder] (/ alone).
/// @param argc the argument count
/// @param argv the arguments
static void cmd_cd(int argc, char **argv) {
    if (!need_drive())
        return;
    FRESULT r = f_chdir(argc > 1 ? argv[1] : "/");
    if (r != FR_OK)
        print_error(argv[1], r);
}

/// pwd.
/// @param argc the argument count
/// @param argv the arguments
static void cmd_pwd(int argc, char **argv) {
    if (!need_drive())
        return;
    char cwd[256];
    if (f_getcwd(cwd, sizeof cwd) == FR_OK)
        printf("%s\n", cwd);
}

/// mkdir folder.
/// @param argc the argument count
/// @param argv the arguments
static void cmd_mkdir(int argc, char **argv) {
    if (!need_drive())
        return;
    if (argc < 2) {
        printf("usage: mkdir folder\n");
        return;
    }
    FRESULT r = f_mkdir(argv[1]);
    if (r != FR_OK)
        print_error(argv[1], r);
}

/// What a copy (or delete) of several files has done so far.
typedef struct {
    /// ask before overwriting; "a" was answered
    ///@{
    bool ask, all;
    ///@}
    bool stop;      ///< Esc or Ctrl-C
    /// Files and folders done.
    ///@{
    int files, dirs;
    ///@}
    long long bytes;  ///< bytes done
} batch_t;

/// The names in a folder, folders marked with a leading '/', read in one go
/// (so changes made while working through them can't upset the listing).
/// @param path the folder
/// @param out set to the names (free each and it)
/// @return their count, or -1
static int folder_names(const char *path, char ***out) {
    static FILINFO info;
    char **names = NULL;
    int n = 0, cap = 0;
    DIR dir;
    if (f_opendir(&dir, path) == FR_OK) {
        while (f_readdir(&dir, &info) == FR_OK && info.fname[0]) {
            if (info.fattrib & (AM_HID | AM_SYS))
                continue;
            if (n == cap) {
                char **bigger = realloc(names, (cap + 16) * sizeof *names);
                if (!bigger)
                    break;
                names = bigger;
                cap += 16;
            }
            size_t len = strlen(info.fname);
            if (!(names[n] = malloc(len + 2)))
                break;
            names[n][0] = (info.fattrib & AM_DIR) ? '/' : ' ';
            memcpy(names[n] + 1, info.fname, len + 1);
            ++n;
        }
        f_closedir(&dir);
    }
    *out = names;
    return n;
}

/// A folder and name joined with a slash.
/// @param folder the folder
/// @param name the name
/// @return the path (malloc'd), or NULL
static char *join(const char *folder, const char *name) {
    size_t a = strlen(folder), b = strlen(name);
    char *p = malloc(a + b + 2);
    if (p) {
        memcpy(p, folder, a);
        size_t k = a;
        if (a && folder[a - 1] != '/')
            p[k++] = '/';
        memcpy(p + k, name, b + 1);
    }
    return p;
}

static bool is_folder(const char *path);
static void into_folder(char *dest, size_t size, const char *to, const char *from);

/// Deletes a folder and everything in it.
/// @param b the batch
/// @param path the folder
/// @param depth folders deep
static void rm_tree(batch_t *b, const char *path, int depth) {
    if (depth > 16) {
        printf("%s: folders nested too deep\n", path);
        return;
    }
    char **names;
    int n = folder_names(path, &names);
    for (int i = 0; i < n && !b->stop; ++i) {
        char *p = join(path, names[i] + 1);
        if (!p)
            break;
        if (names[i][0] == '/') {
            rm_tree(b, p, depth + 1);
        } else {
            FRESULT r = f_unlink(p);
            if (r == FR_OK)
                ++b->files;
            else
                print_error(p, r);
            if (net_stop_key())
                b->stop = true;
        }
        free(p);
    }
    glob_free(names, n);
    if (!b->stop) {
        FRESULT r = f_unlink(path);
        if (r == FR_OK)
            ++b->dirs;
        else
            print_error(path, r);
    }
}

/// rm [-r] [-f] path...: * and ? work; -r deletes folders with what is in
/// them. Asks first (unless -f) when it is more than one plain file.
/// @param argc the argument count
/// @param argv the arguments
static void cmd_rm(int argc, char **argv) {
    if (!need_drive())
        return;
    bool force = false, recurse = false;
    int first = 1;
    for (; first < argc && argv[first][0] == '-' && argv[first][1]; ++first)
        for (const char *o = argv[first] + 1; *o; ++o) {
            if (*o == 'f')
                force = true;
            else if (*o == 'r' || *o == 'R')
                recurse = true;
            else
                first = argc;
        }
    if (argc - first < 1) {
        printf("usage: rm [-r] [-f] path...   (* and ? work; -r: folders and all in them;\n"
               "       -f: don't ask)\n");
        return;
    }
    bool wild = false;
    for (int i = first; i < argc; ++i)
        wild |= glob_wild(argv[i]);
    char **files;
    int n = glob_expand(argv + first, argc - first, recurse, &files);
    if (wild && n && !force) {
        char q[48];
        snprintf(q, sizeof q, "Delete %d %s? (y/N) ", n, recurse ? "files and folders" : "files");
        if (glob_ask(q) != 'y') {
            glob_free(files, n);
            return;
        }
    }
    batch_t b = {0};
    char cwd[300];
    if (f_getcwd(cwd, sizeof cwd) != FR_OK)
        strcpy(cwd, "/");
    for (int i = 0; i < n && !b.stop; ++i) {
        if (recurse && is_folder(files[i])) {
            char abs[300];
            glob_absolute(files[i], abs, sizeof abs);
            if (!strcmp(abs, "/")) {
                printf("rm -r won't delete the whole drive\n");
                continue;
            }
            if (glob_inside(cwd, files[i])) {
                printf("%s: can't delete the current folder (cd out of it first)\n", files[i]);
                continue;
            }
            if (!force && !wild) {
                char q[340];
                snprintf(q, sizeof q, "Delete folder %s and everything in it? (y/N) ", files[i]);
                if (glob_ask(q) != 'y')
                    continue;
            }
            rm_tree(&b, files[i], 0);
            continue;
        }
        FRESULT r = f_unlink(files[i]);
        if (r == FR_OK)
            ++b.files;
        else if (r == FR_DENIED)
            printf("%s: can't delete (folder not empty: rm -r; or read-only)\n", files[i]);
        else
            print_error(files[i], r);
        if (n > 1 && net_stop_key())
            b.stop = true;
    }
    if (b.stop)
        printf("Stopped\n");
    if (n > 1 || wild || recurse)
        printf("%d file%s, %d folder%s deleted\n", b.files, b.files == 1 ? "" : "s", b.dirs,
               b.dirs == 1 ? "" : "s");
    glob_free(files, n);
}

/// mv from... to: rename, or move into the folder `to`; * and ? work.
/// @param argc the argument count
/// @param argv the arguments
static void cmd_mv(int argc, char **argv) {
    if (!need_drive())
        return;
    if (argc < 3) {
        printf("usage: mv from... to   (* and ? work)\n");
        return;
    }
    const char *to = argv[argc - 1];
    bool wild = false;
    for (int i = 1; i < argc - 1; ++i)
        wild |= glob_wild(argv[i]);
    char **files;
    int n = glob_expand(argv + 1, argc - 2, true, &files);
    bool to_dir = is_folder(to), many = n > 1 || wild;
    if (n > 1 && !to_dir) {
        printf("%s isn't a folder (moving several files needs one)\n", to);
        glob_free(files, n);
        return;
    }
    int moved = 0;
    for (int i = 0; i < n; ++i) {
        static char dest[300];
        if (to_dir)
            into_folder(dest, sizeof dest, to, files[i]);
        else
            snprintf(dest, sizeof dest, "%s", to);
        if (glob_same(files[i], dest))
            continue;
        if (many)
            printf("%s -> %s\n", files[i], dest);
        FRESULT r = f_rename(files[i], dest);
        if (r == FR_OK)
            ++moved;
        else if (r == FR_INVALID_DRIVE)
            printf("%s: can't move between the drive and a share (cp, then rm)\n", files[i]);
        else
            print_error(files[i], r);
        if (many && net_stop_key()) {
            printf("Stopped\n");
            break;
        }
    }
    if (many)
        printf("%d moved\n", moved);
    glob_free(files, n);
}

/// cat file: shows a text file, a page at a time.
/// @param argc the argument count
/// @param argv the arguments
static void cmd_cat(int argc, char **argv) {
    if (!need_drive())
        return;
    if (argc < 2) {
        printf("usage: cat file\n");
        return;
    }
    FIL f;
    FRESULT r = f_open(&f, argv[1], FA_READ);
    if (r != FR_OK) {
        print_error(argv[1], r);
        return;
    }
    char buf[512];
    UINT n;
    int col = 0, lines = 0;
    bool stop = false;
    // ANSI/VT100 escape sequences go straight to the console and don't count
    // as columns. Once a file moves the cursor around (full-screen ANSI art),
    // line counts mean nothing, so paging stops.
    int esc = 0;  // 1 after ESC, 2 inside ESC [, 3 one more byte (ESC ( x)
    bool paging = true;
    while (!stop && f_read(&f, buf, sizeof buf, &n) == FR_OK && n > 0) {
        for (UINT i = 0; i < n && !stop; ++i) {
            char c = buf[i];
            if (esc) {
                putchar(c);
                if (esc == 1) {
                    esc = c == '[' ? 2 : (c == '(' || c == ')' || c == '#') ? 3 : 0;
                } else if (esc == 3) {
                    esc = 0;
                } else if (c >= 0x40 && c <= 0x7e) {
                    esc = 0;
                    if (strchr("HfABCDJdG", c))
                        paging = false;
                }
                continue;
            }
            if (c == 0x1a) {
                stop = true;  // Ctrl-Z: end of text; .ANS files put SAUCE data after it
                continue;
            }
            if (c == 0x1b) {
                putchar(c);
                esc = 1;
                continue;
            }
            if (c == '\r')
                continue;
            if (c == '\a' || c == '\b') {
                putchar(c);
                if (c == '\b' && col > 0)
                    --col;
                continue;
            }
            if (c == '\n' || ++col > SCREEN_COLS) {
                col = c == '\n' ? 0 : 1;
                if (paging && ++lines == PAGE_LINES) {
                    if (c == '\n')
                        putchar('\n');
                    lines = 0;
                    stop = !more();
                    if (c == '\n' || stop)
                        continue;
                }
            }
            putchar(c == '\t' || c == '\n' || (unsigned char)c >= 0x20 ? c : '.');
        }
    }
    printf("\x1b[0m\x1b[?25h");  // reset colours and show the cursor
    if (col)
        putchar('\n');
    f_close(&f);
}

/// If `to` is an existing folder, copy into it under the source's name.
/// Copies one file; its size, or -1 (with a message) if it failed.
/// @param src the source's FatFs object
/// @param dst the destination's
/// @param from the source
/// @param to the destination
/// @return its size, or -1
static long long copy_with(FIL *src, FIL *dst, const char *from, const char *to) {
    FRESULT r = f_open(src, from, FA_READ);
    if (r != FR_OK) {
        print_error(from, r);
        return -1;
    }
    r = f_open(dst, to, FA_WRITE | FA_CREATE_ALWAYS);
    if (r != FR_OK) {
        print_error(to, r);
        f_close(src);
        return -1;
    }
    // The buffer in PSRAM, only while copying (the SRAM heap is small).
    enum { COPY_BUF = 4096 };
    static uint8_t *buf;
    if (!buf && !(buf = psram_buffer_alloc(COPY_BUF))) {
        printf("%s: not enough memory\n", from);
        f_close(src);
        f_close(dst);
        return -1;
    }
    UINT got, put;
    long long total = 0;
    while ((r = f_read(src, buf, COPY_BUF, &got)) == FR_OK && got > 0) {
        r = f_write(dst, buf, got, &put);
        if (r != FR_OK || put != got) {
            if (r == FR_OK)
                printf("%s: drive full\n", to);
            total = -1;
            break;
        }
        total += got;
    }
    if (r != FR_OK) {
        print_error(to, r);
        total = -1;
    }
    f_close(src);
    f_close(dst);
    return total;
}

/// Copies one file (into a folder if to is one).
/// @param from the source
/// @param to the destination
/// @return its size, or -1
static long long copy_file(const char *from, const char *to) {
    // The two files' FatFs objects (1.2 KB) only while copying.
    FIL *pair = malloc(2 * sizeof *pair);
    if (!pair) {
        printf("%s: not enough memory\n", from);
        return -1;
    }
    long long n = copy_with(&pair[0], &pair[1], from, to);
    free(pair);
    return n;
}

/// The folder `to` plus the last part of `from`.
/// @param dest the path made
/// @param size room in dest
/// @param to the folder
/// @param from the source
static void into_folder(char *dest, size_t size, const char *to, const char *from) {
    const char *name = strrchr(from, '/');
    name = name ? name + 1 : from;
    size_t n = strlen(to);
    snprintf(dest, size, "%s%s%s", to, n && to[n - 1] == '/' ? "" : "/", name);
}

/// Whether a path is a folder.
/// @param path the path
/// @return it is
static bool is_folder(const char *path) {
    FILINFO info;
    if (!strcmp(path, "/"))
        return true;  // FatFs can't stat the root
    return f_stat(path, &info) == FR_OK && (info.fattrib & AM_DIR);
}

/// Copies a file for cp (asking before overwriting unless -y).
/// @param b the batch
/// @param from the source
/// @param dest the destination
static void cp_file(batch_t *b, const char *from, const char *dest) {
    if (glob_same(from, dest)) {
        printf("%s: can't copy a file onto itself\n", from);
        return;
    }
    FILINFO info;
    if (b->ask && !b->all && f_stat(dest, &info) == FR_OK) {
        char q[340];
        snprintf(q, sizeof q, "Overwrite %s? (y/N/a) ", dest);
        int c = glob_ask(q);
        if (c == 'a')
            b->all = true;
        else if (c != 'y')
            return;
    }
    printf("%s -> %s\n", from, dest);
    long long got = copy_file(from, dest);
    if (got >= 0) {
        ++b->files;
        b->bytes += got;
    }
    if (net_stop_key())
        b->stop = true;
}

/// Copies folder `from` as `to` (made if missing), with everything in it.
/// @param b the batch
/// @param from the folder
/// @param to its copy
/// @param depth folders deep
static void cp_tree(batch_t *b, const char *from, const char *to, int depth) {
    if (depth > 16) {
        printf("%s: folders nested too deep\n", from);
        return;
    }
    FRESULT r = f_mkdir(to);
    if (r == FR_OK)
        ++b->dirs;
    else if (!(r == FR_EXIST && is_folder(to))) {
        print_error(to, r);
        return;
    }
    char **names;
    int n = folder_names(from, &names);
    for (int i = 0; i < n && !b->stop; ++i) {
        char *src = join(from, names[i] + 1), *dst = join(to, names[i] + 1);
        if (src && dst) {
            if (names[i][0] == '/')
                cp_tree(b, src, dst, depth + 1);
            else
                cp_file(b, src, dst);
        }
        free(src);
        free(dst);
    }
    glob_free(names, n);
}

/// cp [-r] [-y] from... to: * and ? work in the last part of a name (cp
/// EXAMPLES/*.BAS /NET); -r copies folders too. With several things to
/// copy, `to` must be a folder.
/// @param argc the argument count
/// @param argv the arguments
static void cmd_cp(int argc, char **argv) {
    if (!need_drive())
        return;
    bool yes = false, recurse = false;
    int first = 1;
    for (; first < argc && argv[first][0] == '-' && argv[first][1]; ++first)
        for (const char *o = argv[first] + 1; *o; ++o) {
            if (*o == 'y')
                yes = true;
            else if (*o == 'r' || *o == 'R')
                recurse = true;
            else
                first = argc;  // unknown: show the usage
        }
    if (argc - first < 2) {
        printf("usage: cp [-r] [-y] from... to   (* and ? work; -r: folders too;\n"
               "       -y: overwrite without asking)\n");
        return;
    }
    const char *to = argv[argc - 1];
    bool wild = false;
    for (int i = first; i < argc - 1; ++i)
        wild |= glob_wild(argv[i]);
    char **files;
    int n = glob_expand(argv + first, argc - 1 - first, recurse, &files);
    bool to_dir = is_folder(to), many = n > 1 || wild || recurse;
    batch_t b = {.ask = many && !yes};
    if (n > 1 && !to_dir) {
        printf("%s isn't a folder (copying several things needs one)\n", to);
        glob_free(files, n);
        return;
    }
    for (int i = 0; i < n && !b.stop; ++i) {
        static char dest[300];
        if (to_dir)
            into_folder(dest, sizeof dest, to, files[i]);
        else
            snprintf(dest, sizeof dest, "%s", to);
        if (is_folder(files[i])) {
            if (!recurse)
                printf("%s is a folder (cp -r copies folders)\n", files[i]);
            else if (glob_inside(dest, files[i]))
                printf("%s: can't copy a folder into itself\n", files[i]);
            else
                cp_tree(&b, files[i], dest, 0);
        } else if (many) {
            cp_file(&b, files[i], dest);
        } else if (glob_same(files[i], dest)) {
            printf("%s: can't copy a file onto itself\n", files[i]);
        } else {
            long long got = copy_file(files[i], dest);  // one file: quietly, as always
            if (got >= 0)
                printf("%llu bytes copied\n", got);
        }
    }
    if (b.stop)
        printf("Stopped\n");
    if (many)
        printf("%d file%s, %d folder%s, %llu bytes copied\n", b.files, b.files == 1 ? "" : "s",
               b.dirs, b.dirs == 1 ? "" : "s", b.bytes);
    glob_free(files, n);
}

/// df: the drive's size and free space.
/// @param argc the argument count
/// @param argv the arguments
static void cmd_df(int argc, char **argv) {
    if (!need_drive())
        return;
    static const char *const types[] = {"", "FAT12", "FAT16", "FAT32", "exFAT"};
    DWORD free_clusters;
    FATFS *pfs;
    if (f_getfree("", &free_clusters, &pfs) != FR_OK)
        return;
    uint64_t cluster = (uint64_t)pfs->csize * 512;
    uint64_t total = (uint64_t)(pfs->n_fatent - 2) * cluster;
    uint64_t free_bytes = (uint64_t)free_clusters * cluster;
    char label[24] = "";
    f_getlabel("", label, NULL);
    printf("%s \"%s\": %llu MB total, %llu MB free, %llu KB clusters\n",
           pfs->fs_type <= 4 ? types[pfs->fs_type] : "?", label, total >> 20, free_bytes >> 20,
           cluster >> 10);
}

extern char __StackLimit;  ///< top of the heap, from the SDK's linker script

/// mem: SRAM and PSRAM use.
/// @param argc the argument count
/// @param argv the arguments
static void cmd_mem(int argc, char **argv) {
    struct mallinfo mi = mallinfo();
    long heap_free = (long)(&__StackLimit - (char *)sbrk(0)) + mi.fordblks;
    printf("SRAM 520 KB: framebuffer 300 KB, heap in use %u KB, heap free %ld KB\n",
           (unsigned)(mi.uordblks / 1024), heap_free / 1024);
    if (heap_free < 16 * 1024)
        printf("\x1b[33mLow on SRAM: big programs may say \"not enough memory\".\x1b[0m\n");
    psram_stats_t ps;
    psram_get_stats(&ps);
    if (ps.total)
        printf("PSRAM %u KB: %u KB in use (%d block%s), %u KB free, largest free %u KB\n",
               (unsigned)(ps.total / 1024), (unsigned)(ps.used / 1024), ps.blocks,
               ps.blocks == 1 ? "" : "s", (unsigned)((ps.total - ps.used) / 1024),
               (unsigned)(ps.largest / 1024));
    else
        printf("PSRAM: not found\n");
}

// Tests a block of PSRAM taken from psram_malloc, so it never touches memory
// in use, and reports the speed.
// ---- memtest soak: a long PSRAM retention test in the background ----------

/// From the PSRAM's cached address to its uncached one.
#define PSRAM_UNCACHED_OFFSET (0x15000000u - 0x11000000u)

static struct {
    uint32_t *p;           ///< cached address of the block under test
    size_t words;  ///< its size in words
    /// When it started, when it ends, and when the next check is.
    ///@{
    uint32_t start_ms, end_ms, next_check_ms;
    ///@}
    uint32_t bad_total;    ///< words found wrong, all checks together
    uint8_t scrub_s;  ///< scrub every this many seconds (0: none)
} soak;  ///< the soak running, if any

/// The pattern a word holds: from its address.
/// @param a the word
/// @return the pattern
static uint32_t soak_pattern(const uint32_t *a) {
    return (uint32_t)(uintptr_t)a * 2654435761u ^ 0x5a5aa5a5u;
}

/// Stops the soak and frees its block.
static void soak_stop(void) {
    psram_scrub(NULL, 0, 0);
    psram_free(soak.p);
    soak.p = NULL;
}

/// Starts a soak: fills a block and checks it now and then.
/// @param minutes how long
/// @param scrub_s scrub every this many seconds (0: none)
static void soak_start(int minutes, int scrub_s) {
    if (soak.p) {
        printf("A soak is already running\n");
        return;
    }
    psram_stats_t st;
    psram_get_stats(&st);
    size_t len = st.largest & ~(size_t)3;
    soak.p = psram_malloc(len);
    if (!soak.p) {
        printf("memtest: no PSRAM to test\n");
        return;
    }
    soak.words = len / 4;
    for (size_t i = 0; i < soak.words; ++i)
        soak.p[i] = soak_pattern(&soak.p[i]);
    xip_cache_clean_all();
    uint32_t now = to_ms_since_boot(get_absolute_time());
    soak.start_ms = now;
    soak.end_ms = now + (uint32_t)minutes * 60000u;
    soak.next_check_ms = now + 60000;
    soak.bad_total = 0;
    soak.scrub_s = scrub_s;
    if (scrub_s)
        psram_scrub(soak.p, len, scrub_s);
    printf("Soak test of %u KB of PSRAM for %d minutes, %s; a line every minute.\n",
           (unsigned)(len / 1024), minutes,
           scrub_s ? "scrubbed" : "not scrubbed");
    if (scrub_s)
        printf("(scrubbing every %d s)\n", scrub_s);
}

/// Called from the main loop.
void shell_task(void) {
    if (!soak.p)
        return;
    uint32_t now = to_ms_since_boot(get_absolute_time());
    if ((int32_t)(now - soak.next_check_ms) < 0)
        return;
    soak.next_check_ms = now + 60000;
    // Read through the uncached window: the chip's own contents.
    const volatile uint32_t *u =
        (const volatile uint32_t *)((uintptr_t)soak.p + PSRAM_UNCACHED_OFFSET);
    uint32_t bad = 0;
    char where[96] = "";
    int wl = 0;
    for (size_t i = 0; i < soak.words; ++i) {
        uint32_t want = soak_pattern(&soak.p[i]), got = u[i];
        if (got == want)
            continue;
        if (++bad <= 3)
            wl += snprintf(where + wl, sizeof where - wl, " %08lx^%08lx",
                           (unsigned long)(uintptr_t)&soak.p[i], (unsigned long)(got ^ want));
        ((volatile uint32_t *)u)[i] = want;  // put it right to see if it fails again
    }
    soak.bad_total += bad;
    bool done = (int32_t)(now - soak.end_ms) >= 0;
    shell_async_begin();
    printf("[memtest soak: %lu min, %lu bad word%s now%s, %lu in all, %lu scrub passes%s]\n",
           (unsigned long)((now - soak.start_ms) / 60000), (unsigned long)bad,
           bad == 1 ? "" : "s", where, (unsigned long)soak.bad_total,
           (unsigned long)psram_scrub_passes(), done ? "; finished" : "");
    shell_async_end();
    if (done)
        soak_stop();
}

/// memtest [soak [minutes [scrub]
/// stop]]: tests the PSRAM.
/// @param argc the argument count
/// @param argv the arguments
static void cmd_memtest(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "soak") == 0) {
        int minutes = argc > 2 ? atoi(argv[2]) : 10;
        int scrub = argc > 3 ? atoi(argv[3]) : 0;
        if (argc > 2 && strcmp(argv[2], "stop") == 0) {
            if (soak.p)
                soak_stop();
            printf("Soak stopped\n");
            return;
        }
        if (minutes <= 0 || scrub < 0) {
            printf("usage: memtest soak [minutes] [scrub seconds, 0 = none] | memtest soak stop\n");
            return;
        }
        soak_start(minutes, scrub);
        return;
    }
    int kb = argc > 1 ? atoi(argv[1]) : 1024;
    if (kb <= 0) {
        printf("usage: memtest [KB]\n");
        return;
    }
    uint8_t *p = psram_malloc((size_t)kb * 1024);
    if (!p) {
        printf("memtest: can't get %d KB of PSRAM\n", kb);
        return;
    }
    uint64_t t = time_us_64();
    void *bad = psram_test(p, (size_t)kb * 1024);
    t = time_us_64() - t;
    if (bad)
        printf("FAILED at %p\n", bad);
    else
        printf("%d KB at %p OK, %u KB/s (write and read back)\n", kb, (void *)p,
               (unsigned)(t ? (uint64_t)kb * 1000000 / t : 0));
    psram_free(p);
}

/// view file: shows a picture full screen until a key is pressed.
/// @param argc the argument count
/// @param argv the arguments
static void cmd_view(int argc, char **argv) {
    if (argc < 2) {
        printf("usage: view file   (BMP, PNG or JPEG)\n");
        return;
    }
    if (!need_drive())
        return;
    image_t img;
    const char *err;
    uint64_t t0 = time_us_64();
    if (!image_load(argv[1], 640, 480, &img, &err)) {
        printf("%s: %s\n", argv[1], err);
        return;
    }
    uint32_t load_ms = (uint32_t)((time_us_64() - t0) / 1000);
    printf("\x1b[?25l");  // hide the cursor: it blinks by drawing on the screen
    fflush(stdout);
    // Keep the text screen to put back afterwards (the console has no copy).
    uint8_t *saved = psram_malloc(DVI_WIDTH * DVI_HEIGHT);
    if (saved)
        memcpy(saved, dvi_framebuf, DVI_WIDTH * DVI_HEIGHT);
    memset(dvi_framebuf, 0, DVI_WIDTH * DVI_HEIGHT);
    int x = 0, y = 0, w = DVI_WIDTH, h = DVI_HEIGHT;
    image_fit(&img, &x, &y, &w, &h);
    t0 = time_us_64();
    image_draw(&img, dvi_framebuf, x, y, w, h, NULL);
    uint32_t draw_ms = (uint32_t)((time_us_64() - t0) / 1000);
    int iw = img.w, ih = img.h;
    image_free(&img);
    while (getchar_timeout_us(100000) == PICO_ERROR_TIMEOUT)
        ;
    while (getchar_timeout_us(20000) != PICO_ERROR_TIMEOUT)  // rest of an arrow key etc.
        ;
    if (saved) {
        memcpy(dvi_framebuf, saved, DVI_WIDTH * DVI_HEIGHT);
        psram_free(saved);
    } else {
        putchar('\f');
    }
    printf("\x1b[?25h%s: %d x %d, loaded in %lu ms, drawn in %lu ms\n", argv[1], iw,
           ih, (unsigned long)load_ms, (unsigned long)draw_ms);
}

/// screenshot [file]: saves the screen (PNG or BMP by the name; the next
/// free /SCREENSHOTS/SHOTnnnn.PNG if none).
/// @param argc the argument count
/// @param argv the arguments
static void cmd_screenshot(int argc, char **argv) {
    if (!need_drive())
        return;
    char name[96];
    if (argc > 1)
        snprintf(name, sizeof name, "%s", argv[1]);
    else
        screenshot_next_name(name, sizeof name);
    const char *err;
    if (screenshot_save(name, NULL, &err))
        printf("Saved the screen to %s\n", name);
    else
        printf("%s: %s\n", name, err);
}

/// edit [file].
/// @param argc the argument count
/// @param argv the arguments
static void cmd_edit(int argc, char **argv) {
    if (!need_drive())
        return;
    editor_run(argc > 1 ? argv[1] : NULL);
}

/// applesoft [file].
/// @param argc the argument count
/// @param argv the arguments
static void cmd_applesoft(int argc, char **argv) {
    if (argc > 1 && !need_drive())
        return;
    app_overlay_load(APP_OV_APPLESOFT);
    applesoft_main(argc > 1 ? argv[1] : NULL);
}

/// apple2e [disk1 [disk2]].
/// @param argc the argument count
/// @param argv the arguments
static void cmd_apple2e(int argc, char **argv) {
    app_overlay_load(APP_OV_APPLE2E);
    apple2e_main(argc, argv);
}

/// c64 [file].
/// @param argc the argument count
/// @param argv the arguments
static void cmd_c64(int argc, char **argv) {
    app_overlay_load(APP_OV_C64);
    c64_main(argc, argv);
}

/// genesis [rom] (when built in).
/// @param argc the argument count
/// @param argv the arguments
static void cmd_genesis(int argc, char **argv) {
#if PICO_OS_GENESIS
    app_overlay_load(APP_OV_GENESIS);
    genesis_main(argc, argv);
#else
    printf("genesis: not in this build (built with -DPICO_OS_GENESIS=OFF)\n");
#endif
}

/// basic [file].
/// @param argc the argument count
/// @param argv the arguments
static void cmd_basic(int argc, char **argv) {
    if (argc > 1 && !need_drive())
        return;
    basic_main(argc > 1 ? argv[1] : NULL);
}

/// cls.
/// @param argc the argument count
/// @param argv the arguments
static void cmd_cls(int argc, char **argv) {
    putchar('\f');
}

/// beep [Hz].
/// @param argc the argument count
/// @param argv the arguments
static void cmd_beep(int argc, char **argv) {
    int hz = argc > 1 ? atoi(argv[1]) : 880;
    if (hz < 20 || hz > 20000) {
        printf("usage: beep [20-20000]\n");
        return;
    }
    audio_beep(hz, 300);
}

/// Shows the VT100 colours and attributes the console supports.
/// @param argc the argument count
/// @param argv the arguments
static void cmd_colors(int argc, char **argv) {
    static const char *const names[] = {"black", "red",     "green", "yellow",
                                        "blue",  "magenta", "cyan",  "white"};
    for (int i = 0; i < 8; ++i)
        printf("\x1b[%dm %-8s\x1b[1m bright \x1b[0m\x1b[%dm\x1b[30m %-8s\x1b[0m\n", 30 + i,
               names[i], 40 + i, names[i]);
    printf("\x1b[1mbold\x1b[0m  \x1b[7mreverse\x1b[0m  normal\n");
}

/// reset [bootsel]: restarts the board through the watchdog, as
/// tools/flash.sh does (so core 1, which runs USB and audio, starts properly
/// too), after leaving WiFi and unmounting the drive. With bootsel it starts
/// the USB bootloader instead, which only helps with the USB port wired to a
/// PC rather than to the hub.
/// @param argc the argument count
/// @param argv the arguments
static void cmd_reset(int argc, char **argv) {
    bool bootsel = argc > 1 && (!strcmp(argv[1], "bootsel") || !strcmp(argv[1], "-b"));
    if (argc > 1 && !bootsel) {
        printf("usage: reset [bootsel]\n");
        return;
    }
    printf(bootsel ? "Starting the USB bootloader...\n" : "Restarting...\n");
    net_shutdown();
    if (mounted) {
        f_unmount("0:");
        mounted = false;
    }
    stdio_flush();
    sleep_ms(200);  // let the debug UART and the WiFi goodbye go out
    if (bootsel)
        reset_usb_boot(0, 0);
    watchdog_reboot(0, 0, 0);
    for (;;)
        tight_loop_contents();
}

// ---- Scripts: run FILE, and /AUTORUN at start-up ---------------------------------------------

/// The start-up script.
#define AUTORUN "/AUTORUN"
static bool autorun_done;  ///< it printed: the start-up screen's lines have moved

bool shell_batch;

/// Esc or Ctrl-C typed: stops a script between its lines.
/// @return stop
static bool script_stop_key(void) {
    int c;
    while ((c = getchar_timeout_us(0)) >= 0)
        if (c == 3 || c == 0x1b)
            return true;
    return false;
}

/// Runs the lines of a file as commands. Blank lines and lines starting
/// with #, REM or :: are comments; "echo off" stops showing the commands
/// (and @ before one hides it); a command that fails doesn't stop the rest.
/// @param path the script
/// @return it ran
static bool run_script(const char *path) {
    static int depth;
    if (depth >= 4) {
        printf("%s: scripts nested too deep\n", path);
        return false;
    }
    FIL f;
    if (f_open(&f, path, FA_READ) != FR_OK) {
        printf("%s: not found\n", path);
        return false;
    }
    ++depth;
    bool was = shell_batch, echo = true;
    shell_batch = true;
    char line[LINE_MAX + 1];
    while (f_gets(line, sizeof line, &f)) {
        char *s = line, *e = line + strlen(line);
        while (e > s && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ' || e[-1] == '\t'))
            *--e = 0;
        while (*s == ' ' || *s == '\t')
            ++s;
        if (!*s || *s == '#' || !strncmp(s, "::", 2) ||
            (!strncasecmp(s, "rem", 3) && (!s[3] || s[3] == ' ')))
            continue;
        bool quiet = *s == '@';
        if (quiet)
            while (*++s == ' ')
                ;
        if (!strcasecmp(s, "echo off") || !strcasecmp(s, "echo on")) {
            echo = !strcasecmp(s, "echo on");
            continue;
        }
        if (script_stop_key()) {
            printf("%s stopped\n", path);
            break;
        }
        if (echo && !quiet)
            printf(HELP_DIM "%s> %s" HELP_END "\n", path, s);
        shell_exec(s);
    }
    f_close(&f);
    shell_batch = was;
    --depth;
    return true;
}

/// run file: runs a script.
/// @param argc the argument count
/// @param argv the arguments
static void cmd_run(int argc, char **argv) {
    if (argc < 2) {
        printf("usage: run FILE\n");
        return;
    }
    if (need_drive())
        run_script(argv[1]);
}

bool shell_autorun(int boot_key) {
    FILINFO fi;
    if (!mounted || f_stat(AUTORUN, &fi) != FR_OK)
        return false;
    if (boot_key == 0x1b || boot_key == 3) {
        printf("Skipping " AUTORUN "\n");
        return true;
    }
    autorun_done = true;
    run_script(AUTORUN);
    return true;
}

/// echo text.
/// @param argc the argument count
/// @param argv the arguments
static void cmd_echo(int argc, char **argv) {
    for (int i = 1; i < argc; ++i)
        printf(i > 1 ? " %s" : "%s", argv[i]);
    putchar('\n');
}

static const struct {
    const char *name;  ///< the command's name
    void (*run)(int argc, char **argv);  ///< what runs it
} commands[] = {
    {"help", cmd_help}, {"ls", cmd_ls},   {"dir", cmd_ls},   {"cd", cmd_cd},
    {"pwd", cmd_pwd},   {"mkdir", cmd_mkdir}, {"rm", cmd_rm}, {"del", cmd_rm},
    {"mv", cmd_mv},     {"ren", cmd_mv},  {"cat", cmd_cat},  {"type", cmd_cat},
    {"cp", cmd_cp},     {"copy", cmd_cp}, {"df", cmd_df},    {"mem", cmd_mem},
    {"cls", cmd_cls},   {"clear", cmd_cls}, {"beep", cmd_beep}, {"echo", cmd_echo}, {"run", cmd_run}, {"source", cmd_run},
    {"colors", cmd_colors}, {"memtest", cmd_memtest},
    {"edit", cmd_edit},     {"ed", cmd_edit},     {"basic", cmd_basic},
    {"applesoft", cmd_applesoft}, {"apple2e", cmd_apple2e}, {"c64", cmd_c64}, {"genesis", cmd_genesis},
    {"wifi", net_cmd_wifi}, {"radio", radio_command}, {"wget", net_cmd_wget}, {"telnet", net_cmd_telnet},
    {"scp", net_cmd_scp},   {"ssh", net_cmd_ssh},   {"mount", smb_cmd_mount}, {"mc", mc_command}, {"tracker", tracker_command}, {"mouse", mouse_command}, {"mod", tracker_command}, {"modplay", tracker_command}, {"umount", smb_cmd_umount},   {"reset", cmd_reset},   {"reboot", cmd_reset},
    {"date", net_cmd_date}, {"ntp", net_cmd_ntp}, {"term", term_command}, {"serial", term_command}, {"view", cmd_view}, {"si5351", si5351_command}, {"clockgen", si5351_command}, {"screenshot", cmd_screenshot},
};  ///< the commands

/// Splits on spaces; "double quotes" keep spaces inside one argument.
/// @param s the line, cut up in place
/// @param argv set to the arguments
/// @param max room in argv
/// @return their count
static int split(char *s, char **argv, int max) {
    int argc = 0;
    while (*s && argc < max) {
        while (*s == ' ')
            ++s;
        if (!*s)
            break;
        if (*s == '"') {
            argv[argc++] = ++s;
            while (*s && *s != '"')
                ++s;
        } else {
            argv[argc++] = s;
            while (*s && *s != ' ')
                ++s;
        }
        if (*s)
            *s++ = 0;
    }
    return argc;
}

/// Runs a command line.
/// @param text the line
static void run_line(char *text) {
    char *argv[LINE_MAX / 2 + 1];  // the most a full line can hold
    int argc = split(text, argv, sizeof argv / sizeof argv[0]);
    if (argc == 0)
        return;
    for (size_t i = 0; i < sizeof commands / sizeof commands[0]; ++i) {
        if (strcmp(argv[0], commands[i].name) == 0) {
            commands[i].run(argc, argv);
            audio_trim();  // (the sound queues a program made, given back)
            return;
        }
    }
    printf("%s: unknown command (try help)\n", argv[0]);
}

// ---- Line editor ----------------------------------------------------------

/// A line's checksum.
/// @param t the line
/// @return the sum
static uint16_t line_sum(const char *t) {
    uint16_t h = 7;
    while (*t)
        h = h * 31 + (uint8_t)*t++;
    return h;
}

/// Adds a line to the history (not a repeat of the last).
/// @param text the line
static void history_add(const char *text) {
    if (!*text)
        return;
    if (!history && !(history = psram_malloc(HISTORY_LEN * sizeof *history)))
        return;
    int last = (history_next + HISTORY_LEN - 1) % HISTORY_LEN;
    if (history_count && strcmp(history[last], text) == 0)
        return;  // don't store the same command twice in a row
    strcpy(history[history_next], text);
    history_sum[history_next] = line_sum(text);
    history_next = (history_next + 1) % HISTORY_LEN;
    if (history_count < HISTORY_LEN)
        ++history_count;
}

/// Shows a history line (0: the new one).
/// @param view lines back
static void history_show(int view) {
    if (view < 0 || view > history_count)
        return;
    history_view = view;
    int k = (history_next + HISTORY_LEN - view) % HISTORY_LEN;
    if (view && (strnlen(history[k], LINE_MAX + 1) > LINE_MAX || line_sum(history[k]) != history_sum[k]))
        history[k][0] = 0, history_sum[k] = line_sum("");  // damaged: forgotten
    replace_line(view ? history[k] : "");
}

/// Inserts text at the cursor.
/// @param t the text
/// @param n its length
static void insert_text(const char *t, int n) {
    if (len + n > LINE_MAX) {
        audio_beep(220, 60);
        return;
    }
    bool at_end = pos == len;
    memmove(line + pos + n, line + pos, len - pos);
    memcpy(line + pos, t, n);
    pos += n;
    len += n;
    if (!at_end) {
        refresh();
        return;
    }
    // Typing (or pasting) at the end: just echo the new characters, which
    // keeps up with fast input where a full redraw wouldn't.
    cursor_to(prompt_len + len - n);
    printf("%.*s", n, t);
    cur_off = prompt_len + len;
    if (cur_off % SCREEN_COLS == 0)
        printf("\r\n");
}

/// Delete: the character at the cursor.
static void delete_at_cursor(void) {
    if (pos >= len)
        return;
    memmove(line + pos, line + pos + 1, len - pos - 1);
    --len;
    refresh();
}

/// Enter: runs the line.
static void enter(void) {
    first_screen = false;
    line[len] = 0;
    leave_line();
    static char copy[LINE_MAX + 1];
    strcpy(copy, line);
    history_add(line);
    history_view = 0;
    len = pos = 0;
    run_line(copy);
    new_prompt();
}

// ---- Tab completion ---------------------------------------------------------
// The first word completes as a command, later ones as file and folder names
// on the drive (relative to the current folder, or inside a typed path).
// A unique match is filled in, followed by "/" for a folder or a space;
// several fill in what they share, and a second Tab lists them.

/// The most candidates.
#define MAX_MATCHES 64
/// Room for their names.
#define NAMES_SIZE  4096

static char *names;  ///< the candidates, each 0-terminated (PSRAM, NAMES_SIZE)
/// Candidates, and bytes of names used.
///@{
static int name_count, names_used;
///@}
/// Each candidate is a folder.
static bool name_is_dir[MAX_MATCHES];

/// Adds a candidate.
/// @param n its name
/// @param dir a folder
static void add_name(const char *n, bool dir) {
    int l = strlen(n) + 1;
    if (!names && !(names = psram_buffer_alloc(NAMES_SIZE)))
        return;
    if (name_count == MAX_MATCHES || names_used + l > NAMES_SIZE)
        return;
    memcpy(names + names_used, n, l);
    name_is_dir[name_count++] = dir;
    names_used += l;
}

/// A candidate's name.
/// @param i which
/// @return it
static const char *name_at(int i) {
    const char *n = names;
    while (i--)
        n += strlen(n) + 1;
    return n;
}

/// Lists the candidates under the line.
static void list_candidates(void) {
    leave_line();
    int width = 0;
    for (int i = 0; i < name_count; ++i) {
        int w = strlen(name_at(i)) + (name_is_dir[i] ? 1 : 0);
        if (w > width)
            width = w;
    }
    width += 2;
    int cols = SCREEN_COLS / width;
    if (cols < 1)
        cols = 1;
    for (int i = 0; i < name_count; ++i) {
        const char *n = name_at(i);
        printf("%s%s", n, name_is_dir[i] ? "/" : "");
        if (i % cols == cols - 1 || i == name_count - 1)
            printf("\r\n");
        else
            printf("%*s", width - (int)strlen(n) - (name_is_dir[i] ? 1 : 0), "");
    }
    if (name_count == MAX_MATCHES)
        printf("(and maybe more)\r\n");
    new_prompt();
}

/// Tab: completes the word at the cursor (twice: lists the candidates).
/// @param twice the second Tab
static void complete(bool twice) {
    // The word under the cursor: back to an unquoted space, or to an open quote.
    int ws = 0;
    bool quoted = false;
    for (int i = 0; i < pos; ++i) {
        if (line[i] == '"') {
            quoted = !quoted;
            ws = i + 1;
        } else if (line[i] == ' ' && !quoted) {
            ws = i + 1;
        }
    }
    bool first_word = true;
    for (int i = 0; i < ws; ++i)
        if (line[i] != ' ' && line[i] != '"')
            first_word = false;
    char word[LINE_MAX + 1];
    snprintf(word, sizeof word, "%.*s", pos - ws, line + ws);
    // For a path, the folder part stays as typed; the last part completes.
    const char *slash = strrchr(word, '/');
    const char *prefix = slash ? slash + 1 : word;
    int plen = strlen(prefix);

    name_count = names_used = 0;
    if (first_word) {
        for (size_t i = 0; i < sizeof commands / sizeof commands[0]; ++i)
            if (!strncasecmp(commands[i].name, prefix, plen))
                add_name(commands[i].name, false);
    } else {
        if (!mounted) {
            audio_beep(220, 60);
            return;
        }
        char dir[LINE_MAX + 1] = ".";
        if (slash)
            snprintf(dir, sizeof dir, "%.*s", (int)(slash - word) ? (int)(slash - word) : 1,
                     slash == word ? "/" : word);
        DIR d;
        FILINFO info;
        if (f_opendir(&d, dir) == FR_OK) {
            while (f_readdir(&d, &info) == FR_OK && info.fname[0]) {
                if (info.fattrib & (AM_HID | AM_SYS))
                    continue;
                if (!strncasecmp(info.fname, prefix, plen))
                    add_name(info.fname, info.fattrib & AM_DIR);
            }
            f_closedir(&d);
        }
    }
    if (!name_count) {
        audio_beep(220, 60);
        return;
    }
    // What all the candidates share beyond the prefix (case-insensitive;
    // the first one's spelling is used).
    const char *first = name_at(0);
    int common = strlen(first);
    for (int i = 1; i < name_count; ++i) {
        const char *n = name_at(i);
        int k = 0;
        while (k < common && n[k] && tolower((unsigned char)n[k]) == tolower((unsigned char)first[k]))
            ++k;
        common = k;
    }
    char add[LINE_MAX + 3];
    int n = 0;
    // Replace the typed prefix too, so the name gets its stored case.
    pos -= plen;
    memmove(line + pos, line + pos + plen, len - pos - plen);
    len -= plen;
    bool needs_quote = !quoted && !first_word && memchr(first, ' ', common);
    if (needs_quote) {
        // Open a quote at the start of the word.
        memmove(line + ws + 1, line + ws, len - ws);
        line[ws] = '"';
        ++len;
        ++pos;
        quoted = true;
    }
    n = snprintf(add, sizeof add, "%.*s", common, first);
    if (name_count == 1) {
        if (name_is_dir[0]) {
            add[n++] = '/';
        } else {
            if (quoted)
                add[n++] = '"';
            add[n++] = ' ';
        }
        add[n] = 0;
    }
    insert_text(add, n);
    if (name_count > 1 && common <= plen) {
        if (twice)
            list_candidates();
        else
            audio_beep(440, 30);
    }
}

/// An escape sequence's key: arrows, Home, End, Del.
/// @param final its final character
static void escape_key(char final) {
    switch (final) {
    case 'A': history_show(history_view + 1); break;  // up
    case 'B': history_show(history_view - 1); break;  // down
    case 'C':                                         // right
        if (pos < len)
            cursor_to(prompt_len + ++pos);
        break;
    case 'D':  // left
        if (pos > 0)
            cursor_to(prompt_len + --pos);
        break;
    case 'H':  // home
        pos = 0;
        cursor_to(prompt_len);
        break;
    case 'F':  // end
        pos = len;
        cursor_to(prompt_len + len);
        break;
    case '~':
        if (esc_param == '3')
            delete_at_cursor();
        else if (esc_param == '1' || esc_param == '7')
            escape_key('H');
        else if (esc_param == '4' || esc_param == '8')
            escape_key('F');
        break;
    }
}

void shell_key(int c) {
    bool tab = c == '\t';
    bool second_tab = tab && last_was_tab;
    last_was_tab = tab;
    if (esc_state == 1) {
        esc_state = (c == '[' || c == 'O') ? 2 : 0;
        esc_param = 0;
        return;
    }
    if (esc_state == 2) {
        if (c >= '0' && c <= '9') {
            esc_param = c;
            return;
        }
        esc_state = 0;
        escape_key(c);
        fflush(stdout);
        return;
    }

    switch (c) {
    case 0x1b:
        esc_state = 1;
        return;
    case '\t':
        complete(second_tab);
        break;
    case '\r':
    case '\n':
        enter();
        break;
    case '\b':
    case 0x7f:
        if (pos > 0) {
            --pos;
            delete_at_cursor();
        }
        break;
    case 3:  // Ctrl-C: drop the line
        cursor_to(prompt_len + len);
        printf("^C\r\n");
        len = pos = 0;
        history_view = 0;
        new_prompt();
        break;
    case 7:  // Ctrl-G
        audio_beep(880, 200);
        break;
    case 12:  // Ctrl-L: clear and redraw
        putchar('\f');
        new_prompt();
        break;
    default:
        if (c >= 0x20 && c < 0x7f) {
            char ch = c;
            insert_text(&ch, 1);
        }
    }
    fflush(stdout);
}


void shell_async_begin(void) {
    // Leave the line being typed where it is and start the message on the
    // next row; shell_async_end() then repeats the prompt and the line.
    if (!started)
        return;
    first_screen = false;
    if (len == 0 && prompt_len < SCREEN_COLS) {
        // Nothing typed: the message takes the prompt's place, so the
        // screen doesn't fill with empty prompts.
        printf("\r\x1b[K");
        cur_off = 0;
    } else {
        leave_line();
    }
}

void shell_async_end(void) {
    if (started)
        new_prompt();
    fflush(stdout);
}

/// "1.5 GB" or "350 MB".
/// @param bytes the size
/// @param out the text
/// @param n room in out
static void format_size(uint64_t bytes, char *out, size_t n) {
    if (bytes >= (1ull << 30))
        snprintf(out, n, "%llu.%llu GB", bytes >> 30, ((bytes * 10) >> 30) % 10);
    else
        snprintf(out, n, "%llu MB", bytes >> 20);
}

/// Prints "64.0 GB, 1.2 GB used, 62.8 GB free" for the mounted drive.
static void print_drive_summary(void) {
    DWORD free_clusters;
    FATFS *pfs;
    uint64_t t = time_us_64();
    FRESULT r = f_getfree("", &free_clusters, &pfs);
    debug_printf("drive: free space found in %u ms\n", (unsigned)((time_us_64() - t) / 1000));
    if (r != FR_OK) {
        printf("ready (free space unknown)");
        return;
    }
    uint64_t cluster = (uint64_t)pfs->csize * 512;
    uint64_t total = (uint64_t)(pfs->n_fatent - 2) * cluster;
    uint64_t free_bytes = (uint64_t)free_clusters * cluster;
    char a[24], b[24], c[24];
    format_size(total, a, sizeof a);
    format_size(total - free_bytes, b, sizeof b);
    format_size(free_bytes, c, sizeof c);
    printf("%s, %s used, %s free", a, b, c);
}

void shell_drive_changed(bool present) {
    shell_async_begin();
    if (present) {
        FRESULT r = f_mount(&fs, "0:", 1);
        mounted = r == FR_OK;
        // At start-up main() has printed the "Drive" label.
        printf(started ? "[USB drive: " : "\x1b[97m");
        if (mounted) {
            f_chdir("/");
            print_drive_summary();
        } else {
            printf("can't read it, %s", fresult_name(r));
        }
        printf(started ? "]\n" : "\x1b[0m\n");
        if (mounted)
            net_drive_ready();
    } else {
        f_unmount("0:");
        mounted = false;
        printf("[USB drive removed]\n");
    }
    shell_async_end();
}

bool shell_drive_mounted(void) {
    return mounted;
}

void shell_start(void) {
    started = true;
    first_screen = !autorun_done;
    printf("\n  \x1b[90mType help for commands.\x1b[0m\n\n");
    new_prompt();
    fflush(stdout);
}

bool shell_update_wifi_line(void) {
    if (!first_screen)
        return false;
    // Up from the prompt's first row to the WiFi line, rewrite it, and come
    // back to where the cursor was in the line being typed.
    cursor_to(0);
    printf("\x1b[%dA\r  \x1b[90m%-10s\x1b[0m", STATUS_ROWS_UP, "WiFi");
    net_print_status();
    printf("\x1b[K\r\x1b[%dB", STATUS_ROWS_UP);
    cur_off = 0;
    cursor_to(prompt_len + pos);
    fflush(stdout);
    return true;
}

void shell_exec(const char *line) {
    char copy[LINE_MAX + 1];
    snprintf(copy, sizeof copy, "%s", line);
    run_line(copy);
}

bool shell_run_command(const char *line) {
    char copy[LINE_MAX + 1];
    snprintf(copy, sizeof copy, "%s", line);
    char *argv[LINE_MAX / 2 + 1];
    char scratch[LINE_MAX + 1];
    strcpy(scratch, copy);
    int argc = split(scratch, argv, sizeof argv / sizeof argv[0]);
    if (argc && (!strcmp(argv[0], "basic") || !strcmp(argv[0], "edit") || !strcmp(argv[0], "ed") ||
                 !strcmp(argv[0], "applesoft") || !strcmp(argv[0], "apple2e") ||
                 !strcmp(argv[0], "c64") || !strcmp(argv[0], "genesis")))
        return false;
    run_line(copy);
    return true;
}
