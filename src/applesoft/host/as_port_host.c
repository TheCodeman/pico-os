/**
 * @file as_port_host.c
 * @brief The Applesoft port layer on a PC, for testing.
 *
 *   applesoft [-i input.txt] [-s screen.ppm] [-d start-end file]... [program]
 *
 * With -i, keys come from the file and the run ends when it has all been
 * read and the interpreter waits for more; the transcript (everything
 * printed from the first keyboard read on, as the oracle records it) goes to
 * stdout. Without -i, stdin is the keyboard (raw mode on a terminal).
 * -s saves the 640x480 screen as a PPM when the run ends; -d dumps the
 * emulated memory. Files are in the current directory.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "applesoft.h"
#include "as_port.h"

static uint8_t framebuf[ASP_FB_WIDTH * ASP_FB_HEIGHT];
static const char *script;
static size_t script_len, script_pos;
static bool scripted, transcript_on, raw_tty;
static struct termios saved_tty;
static const char *file_error = "";

void *asp_alloc(size_t n) {
    return calloc(1, n);
}

void asp_free(void *p) {
    free(p);
}

uint8_t *asp_framebuffer(void) {
    return framebuf;
}

void asp_screen_begin(void) {
}

void asp_screen_end(void) {
}

void asp_mirror(int c) {
    if (scripted && !transcript_on)
        return;
    putchar(c == '\r' ? '\n' : c);
    if (!scripted)
        fflush(stdout);
}

int asp_key(uint32_t timeout_us) {
    if (scripted) {
        // Like the oracle, keys only start coming once something waits for
        // one (a poll before that sees none).
        if (!transcript_on && timeout_us == 0)
            return -1;
        transcript_on = true;
        if (script_pos < script_len) {
            char c = script[script_pos++];
            return c == '\n' ? '\r' : (uint8_t)c;
        }
        return -1;
    }
    fflush(stdout);
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(0, &fds);
    struct timeval tv = {timeout_us / 1000000, timeout_us % 1000000};
    if (select(1, &fds, NULL, NULL, &tv) <= 0)
        return -1;
    unsigned char c;
    if (read(0, &c, 1) != 1)
        return -1;
    return c == '\n' ? '\r' : c;
}

bool asp_input_done(void) {
    return scripted && script_pos >= script_len;
}

int asp_paddle(int n) {
    return 127;
}

bool asp_button(int n) {
    return false;
}

uint64_t asp_micros(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + ts.tv_nsec / 1000;
}

void asp_sleep_us(uint32_t us) {
    if (!scripted)
        usleep(us);
}

void asp_tone(unsigned freq_hz, unsigned ms) {
}

void asp_speaker_click(uint64_t when_us) {
}

bool asp_stack_low(void) {
    return false;
}

// ---- Files ---------------------------------------------------------------------

struct asp_file {
    FILE *f;
};

asp_file *asp_open(const char *name, int mode) {
    static const char *const modes[] = {"rb", "wb", "ab", "r+b"};
    FILE *f = fopen(name, modes[mode]);
    if (!f && mode == ASP_UPDATE)
        f = fopen(name, "w+b");
    if (!f) {
        file_error = strerror(errno);
        return NULL;
    }
    asp_file *af = malloc(sizeof *af);
    af->f = f;
    return af;
}

int asp_read(asp_file *f, void *buf, int n) {
    size_t r = fread(buf, 1, n, f->f);
    return ferror(f->f) ? -1 : (int)r;
}

int asp_write(asp_file *f, const void *buf, int n) {
    return fwrite(buf, 1, n, f->f) == (size_t)n ? n : -1;
}

bool asp_seek(asp_file *f, uint32_t pos) {
    return fseek(f->f, pos, SEEK_SET) == 0;
}

uint32_t asp_tell(asp_file *f) {
    return (uint32_t)ftell(f->f);
}

uint32_t asp_size(asp_file *f) {
    long here = ftell(f->f);
    fseek(f->f, 0, SEEK_END);
    long size = ftell(f->f);
    fseek(f->f, here, SEEK_SET);
    return (uint32_t)size;
}

bool asp_close(asp_file *f) {
    bool ok = fclose(f->f) == 0;
    free(f);
    return ok;
}

bool asp_exists(const char *name) {
    struct stat st;
    return stat(name, &st) == 0;
}

bool asp_remove(const char *name) {
    if (remove(name) == 0)
        return true;
    file_error = strerror(errno);
    return false;
}

bool asp_rename(const char *from, const char *to) {
    if (rename(from, to) == 0)
        return true;
    file_error = strerror(errno);
    return false;
}

bool asp_list(void (*fn)(void *ctx, const char *name, uint32_t size, bool is_dir), void *ctx) {
    DIR *d = opendir(".");
    if (!d) {
        file_error = strerror(errno);
        return false;
    }
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.')
            continue;
        struct stat st;
        if (stat(e->d_name, &st) == 0)
            fn(ctx, e->d_name, (uint32_t)st.st_size, S_ISDIR(st.st_mode));
    }
    closedir(d);
    return true;
}

const char *asp_file_error(void) {
    return file_error;
}

// ---- The test program ------------------------------------------------------------

static struct {
    ///@{
    unsigned start, end;
    ///@}
    const char *file;
} dumps[8];
static int ndumps;
static const char *screenshot;

static void save_ppm(const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f)
        return;
    fprintf(f, "P6\n%d %d\n255\n", ASP_FB_WIDTH, ASP_FB_HEIGHT);
    for (int i = 0; i < ASP_FB_WIDTH * ASP_FB_HEIGHT; ++i) {
        uint8_t p = framebuf[i];
        fputc((p >> 5) * 255 / 7, f);
        fputc(((p >> 2) & 7) * 255 / 7, f);
        fputc((p & 3) * 255 / 3, f);
    }
    fclose(f);
}

static void on_exit_dump(const uint8_t *ram) {
    for (int i = 0; i < ndumps; ++i) {
        FILE *f = fopen(dumps[i].file, "wb");
        if (!f)
            continue;
        fwrite(ram + dumps[i].start, 1, dumps[i].end - dumps[i].start + 1, f);
        fclose(f);
    }
    if (screenshot)
        save_ppm(screenshot);
}

static void restore_tty(void) {
    if (raw_tty)
        tcsetattr(0, TCSANOW, &saved_tty);
}

int main(int argc, char **argv) {
    const char *program = NULL;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "-i") && i + 1 < argc) {
            FILE *f = fopen(argv[++i], "rb");
            if (!f) {
                perror(argv[i]);
                return 1;
            }
            static char buf[1 << 20];
            script_len = fread(buf, 1, sizeof buf, f);
            fclose(f);
            script = buf;
            scripted = true;
        } else if (!strcmp(argv[i], "-s") && i + 1 < argc) {
            screenshot = argv[++i];
        } else if (!strcmp(argv[i], "-d") && i + 2 < argc && ndumps < 8) {
            if (sscanf(argv[i + 1], "%x-%x", &dumps[ndumps].start, &dumps[ndumps].end) != 2 ||
                dumps[ndumps].end > 0xffff || dumps[ndumps].start > dumps[ndumps].end)
                return 2;
            dumps[ndumps++].file = argv[i + 2];
            i += 2;
        } else if (argv[i][0] != '-') {
            program = argv[i];
        } else {
            fprintf(stderr, "usage: applesoft [-i input] [-s screen.ppm] [-d start-end file]... "
                            "[program]\n");
            return 2;
        }
    }
    if (!scripted && isatty(0)) {
        struct termios t;
        tcgetattr(0, &saved_tty);
        t = saved_tty;
        t.c_lflag &= ~(ICANON | ECHO | ISIG);
        tcsetattr(0, TCSANOW, &t);
        raw_tty = true;
        atexit(restore_tty);
    }
    applesoft_on_exit = on_exit_dump;
    applesoft_main(program);
    fflush(stdout);
    return 0;
}
