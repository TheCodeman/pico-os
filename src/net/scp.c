/**
 * @file scp.c
 * @brief scp: copies files to and from an SSH server, using the classic scp protocol: the server runs "scp -f" (sending) or "scp -t" (receiving) and the two sides trade one-line headers ("C0644 size name", "D0755 0 dir", "E") with a zero byte to acknowledge each step.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ff.h"
#include "net.h"
#include "pico/stdlib.h"
#include "psram_heap.h"
#include "ssh.h"

#define CHUNK    16384  ///< the drive writes big blocks much faster
#define LINE_MAX 1100
#define PATH_MAX 256

typedef struct {
    ssh_t *ssh;
    bool recursive;
    uint8_t *buf;
    /// Progress of the current file.
    const char *name;
    ///@{
    uint64_t start_us, last_us;
    ///@}
    ///@{
    uint32_t done, size;
    ///@}
    uint32_t files;
    ///@{
    uint64_t total_bytes, total_us;
    ///@}
} scp_t;

// ---- Progress -----------------------------------------------------------------

static void progress_start(scp_t *c, const char *name, uint32_t size) {
    c->name = strrchr(name, '/') ? strrchr(name, '/') + 1 : name;
    c->size = size;
    c->done = 0;
    c->start_us = c->last_us = time_us_64();
}

static void progress_show(scp_t *c, bool final) {
    uint64_t now = time_us_64();
    if (!final && now - c->last_us < 250000)
        return;
    c->last_us = now;
    uint64_t us = now - c->start_us;
    unsigned rate = us ? (unsigned)((uint64_t)c->done * 1000000 / us / 1024) : 0;
    printf("\r%-24.24s %3u%% %8lu KB %5u KB/s", c->name,
           c->size ? (unsigned)((uint64_t)c->done * 100 / c->size) : 100,
           (unsigned long)((c->done + 1023) / 1024), rate);
    if (final) {
        printf("\n");
        ++c->files;
        c->total_bytes += c->done;
        c->total_us += us;
    }
    fflush(stdout);
}

// ---- Talking to the remote scp -----------------------------------------------------

static bool read_exact(scp_t *c, void *buf, size_t n) {
    uint8_t *p = buf;
    while (n) {
        int got = ssh_read(c->ssh, p, n);
        if (got <= 0) {
            if (got == 0)
                printf("\nThe server ended the copy early\n");
            return false;
        }
        p += got;
        n -= got;
    }
    return true;
}

/// Reads a header line (without its newline).
static bool read_line(scp_t *c, char *line, size_t max) {
    size_t n = 0;
    for (;;) {
        char ch;
        if (!read_exact(c, &ch, 1))
            return false;
        if (ch == '\n')
            break;
        if (n < max - 1)
            line[n++] = ch;
    }
    line[n] = 0;
    return true;
}

/// The remote's answer to a step: 0 is fine; 1 (warning) and 2 (error) come
/// with a message.
static bool read_ack(scp_t *c) {
    uint8_t b;
    if (!read_exact(c, &b, 1))
        return false;
    if (b == 0)
        return true;
    char msg[LINE_MAX];
    if (b == 1 || b == 2) {
        if (read_line(c, msg, sizeof msg))
            printf("\nscp (server): %s\n", msg);
    } else {
        msg[0] = b;
        if (read_line(c, msg + 1, sizeof msg - 1))
            printf("\nscp: the server said: %s\n", msg);
    }
    return false;
}

static bool send_ack(scp_t *c) {
    return ssh_write(c->ssh, "", 1);
}

/// Tells the remote about a problem on our side.
static void send_error(scp_t *c, const char *why) {
    char line[200];
    int n = snprintf(line, sizeof line, "\001scp: %s\n", why);
    ssh_write(c->ssh, line, n);
}

static bool is_dir(const char *path) {
    if (!strcmp(path, ".") || !strcmp(path, "/") || !path[0])
        return true;
    FILINFO fi;
    return f_stat(path, &fi) == FR_OK && (fi.fattrib & AM_DIR);
}

/// dir/name in out (PATH_MAX bytes). False if it doesn't fit.
static bool join(char *out, const char *dir, const char *name) {
    size_t n = strlen(dir);
    int len = snprintf(out, PATH_MAX, "%s%s%s", dir, n && dir[n - 1] != '/' ? "/" : "", name);
    if (len < 0 || len >= PATH_MAX) {
        printf("Path too long: %s/%s\n", dir, name);
        return false;
    }
    return true;
}

// ---- Downloading ---------------------------------------------------------------------

/// Receives one file's data into `path`.
static bool sink_file(scp_t *c, const char *path, uint32_t size) {
    FIL f;
    FRESULT r = f_open(&f, path, FA_WRITE | FA_CREATE_ALWAYS);
    if (r != FR_OK) {
        printf("Can't create %s (error %d)\n", path, r);
        send_error(c, "can't create the file");
        return false;
    }
    progress_start(c, path, size);
    bool ok = send_ack(c);
    size_t have = 0;
    while (ok && c->done < size) {
        uint32_t want = size - c->done;
        if (want > CHUNK - have)
            want = CHUNK - have;
        int got = ssh_read(c->ssh, c->buf + have, want);
        if (got <= 0) {
            if (got == 0)
                printf("\nThe server ended the copy early\n");
            ok = false;
            break;
        }
        have += got;
        c->done += got;
        if (have == CHUNK || c->done == size) {
            UINT w;
            r = f_write(&f, c->buf, have, &w);
            if (r != FR_OK || w != have) {
                printf("\nCan't write %s (drive full?)\n", path);
                ok = false;
            }
            have = 0;
        }
        progress_show(c, false);
    }
    if (f_close(&f) != FR_OK)
        ok = false;
    if (ok)
        ok = read_ack(c) && send_ack(c);
    if (ok) {
        progress_show(c, true);
    } else {
        f_unlink(path);  // no half files left behind
        printf("%s not copied\n", path);
    }
    return ok;
}

/// Runs the receiving side: headers and files until the server is done.
static bool sink(scp_t *c, const char *target) {
    char line[LINE_MAX], dir[PATH_MAX];
    snprintf(dir, sizeof dir, "%s", target);
    int depth = 0;
    bool target_is_dir = is_dir(target), got_any = false;
    if (!send_ack(c))
        return false;
    for (;;) {
        uint8_t type;
        int got = ssh_read(c->ssh, &type, 1);
        if (got == 0)
            return got_any || (printf("Nothing was copied\n"), false);
        if (got < 0)
            return false;
        if (!read_line(c, line, sizeof line))
            return false;
        if (type == 1 || type == 2) {  // a message from the remote scp
            printf("scp (server): %s\n", line);
            if (type == 2)
                return false;
            continue;
        }
        if (type == 'T') {  // times: we don't keep them
            if (!send_ack(c))
                return false;
            continue;
        }
        if (type == 'E') {
            if (depth == 0)
                return false;
            --depth;
            char *slash = strrchr(dir, '/');
            if (slash)
                *slash = 0;
            if (!send_ack(c))
                return false;
            continue;
        }
        unsigned mode;
        unsigned long size;
        int name_at = 0;
        if ((type != 'C' && type != 'D') ||
            sscanf(line, "%o %lu %n", &mode, &size, &name_at) < 2 || !name_at) {
            printf("scp: confusing header from the server: %c%s\n", type, line);
            return false;
        }
        const char *name = line + name_at;
        if (!name[0] || strchr(name, '/') || !strcmp(name, "..") || !strcmp(name, ".")) {
            printf("scp: the server sent a bad file name: %s\n", name);
            send_error(c, "bad file name");
            return false;
        }
        char path[PATH_MAX];
        if (depth || target_is_dir) {
            if (!join(path, dir, name)) {
                send_error(c, "path too long");
                return false;
            }
        } else
            snprintf(path, sizeof path, "%s", dir);
        if (type == 'D') {
            if (!c->recursive) {
                send_error(c, "use -r for folders");
                return false;
            }
            FRESULT r = f_mkdir(path);
            if (r != FR_OK && r != FR_EXIST) {
                printf("Can't make folder %s (error %d)\n", path, r);
                send_error(c, "can't make the folder");
                return false;
            }
            snprintf(dir, sizeof dir, "%s", path);
            ++depth;
            if (!send_ack(c))
                return false;
            continue;
        }
        if (size > 0xffffffffu) {
            send_error(c, "file too big");
            return false;
        }
        if (!sink_file(c, path, size))
            return false;
        got_any = true;
    }
}

// ---- Uploading -------------------------------------------------------------------------

static bool source_file(scp_t *c, const char *path, const char *name, uint32_t size) {
    FIL f;
    FRESULT r = f_open(&f, path, FA_READ);
    if (r != FR_OK) {
        printf("Can't open %s (error %d)\n", path, r);
        return false;
    }
    char line[PATH_MAX + 40];
    int n = snprintf(line, sizeof line, "C0644 %lu %s\n", (unsigned long)size, name);
    bool ok = ssh_write(c->ssh, line, n) && read_ack(c);
    progress_start(c, path, size);
    while (ok && c->done < size) {
        UINT got;
        r = f_read(&f, c->buf, CHUNK, &got);
        if (r != FR_OK || !got) {
            printf("\nCan't read %s\n", path);
            ok = false;
            break;
        }
        ok = ssh_write(c->ssh, c->buf, got);
        c->done += got;
        progress_show(c, false);
    }
    f_close(&f);
    if (ok)
        ok = send_ack(c) && read_ack(c);
    if (ok)
        progress_show(c, true);
    else
        printf("%s not copied\n", path);
    return ok;
}

static bool source_dir(scp_t *c, const char *path, const char *name) {
    char line[PATH_MAX + 20];
    int n = snprintf(line, sizeof line, "D0755 0 %s\n", name);
    if (!ssh_write(c->ssh, line, n) || !read_ack(c))
        return false;
    DIR d;
    if (f_opendir(&d, path) != FR_OK) {
        printf("Can't read folder %s\n", path);
        return false;
    }
    FILINFO fi;
    bool ok = true;
    while (ok && f_readdir(&d, &fi) == FR_OK && fi.fname[0]) {
        char sub[PATH_MAX];
        if (!join(sub, path, fi.fname))
            ok = false;
        else if (fi.fattrib & AM_DIR)
            ok = source_dir(c, sub, fi.fname);
        else
            ok = source_file(c, sub, fi.fname, fi.fsize);
    }
    f_closedir(&d);
    return ok && ssh_write(c->ssh, "E\n", 2) && read_ack(c);
}

static bool source(scp_t *c, const char *path) {
    if (!read_ack(c))
        return false;
    const char *name = strrchr(path, '/') ? strrchr(path, '/') + 1 : path;
    if (is_dir(path)) {
        if (!c->recursive) {
            printf("%s is a folder: use scp -r\n", path);
            return false;
        }
        return source_dir(c, path, name[0] ? name : "files");
    }
    FILINFO fi;
    if (f_stat(path, &fi) != FR_OK) {
        printf("No such file: %s\n", path);
        return false;
    }
    return source_file(c, path, name, fi.fsize);
}

// ---- The command -----------------------------------------------------------------------

/// Splits "[user@]host:path". False if `arg` isn't remote.
static bool parse_remote(const char *arg, char *user, char *host, char *path) {
    const char *colon = strchr(arg, ':');
    if (!colon || colon == arg)
        return false;
    const char *at = memchr(arg, '@', colon - arg);
    const char *h = arg;
    user[0] = 0;
    if (at) {
        snprintf(user, 64, "%.*s", (int)(at - arg), arg);
        h = at + 1;
    }
    snprintf(host, 128, "%.*s", (int)(colon - h), h);
    snprintf(path, PATH_MAX, "%s", colon[1] ? colon + 1 : ".");
    return host[0] != 0;
}

/// The remote path quoted for the server's shell.
static void quote(char *out, size_t max, const char *path) {
    size_t n = 0;
    out[n++] = '\'';
    for (; *path && n < max - 6; ++path) {
        if (*path == '\'') {
            memcpy(out + n, "'\\''", 4);
            n += 4;
        } else {
            out[n++] = *path;
        }
    }
    out[n++] = '\'';
    out[n] = 0;
}

static void usage(void) {
    printf("usage: scp [-P port] [-r] user@host:file local\n"
           "       scp [-P port] [-r] local user@host:file\n"
           "Copies files over SSH. -r copies folders. The password is asked for;\n"
           "server keys are remembered in /SSH/KNOWN_HOSTS.\n");
}

void net_cmd_scp(int argc, char **argv) {
    uint16_t port = 22;
    bool recursive = false;
    int i = 1;
    for (; i < argc && argv[i][0] == '-'; ++i) {
        if (!strcmp(argv[i], "-P") && i + 1 < argc) {
            port = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "-r")) {
            recursive = true;
        } else {
            usage();
            return;
        }
    }
    if (argc - i != 2 || !port) {
        usage();
        return;
    }
    const char *from = argv[i], *to = argv[i + 1];
    char user[64], host[128], rpath[PATH_MAX];
    bool upload;
    if (parse_remote(from, user, host, rpath) && !strchr(to, '@')) {
        upload = false;
    } else if (parse_remote(to, user, host, rpath)) {
        upload = true;
    } else {
        printf("scp: one side must be remote: user@host:path\n");
        return;
    }
    if (!user[0]) {
        printf("scp: give the user name: user@%s:...\n", host);
        return;
    }
    if (upload && !is_dir(from)) {
        FILINFO fi;
        if (f_stat(from, &fi) != FR_OK) {
            printf("No such file: %s\n", from);
            return;
        }
    }
    if (!net_need_link())
        return;

    scp_t c = {.recursive = recursive};
    c.buf = psram_buffer_alloc(CHUNK);
    if (!c.buf) {
        printf("scp: not enough memory\n");
        return;
    }
    c.ssh = ssh_open(host, port, user);
    bool ok = false;
    if (c.ssh) {
        char quoted[PATH_MAX * 4 + 3], cmd[PATH_MAX * 4 + 20];
        quote(quoted, sizeof quoted, rpath);
        snprintf(cmd, sizeof cmd, "scp %s%s %s", recursive ? "-r " : "",
                 upload ? "-t" : "-f", quoted);
        if (ssh_exec(c.ssh, cmd))
            ok = upload ? source(&c, from) : sink(&c, to);
        if (ok && upload) {
            ssh_send_eof(c.ssh);
            uint8_t b;
            while (ssh_read(c.ssh, &b, 1) > 0)  // wait for the remote to finish
                ;
        }
        ssh_close(c.ssh);
    }
    psram_buffer_free(c.buf);
    if (c.files > 1 || (ok && c.files)) {
        unsigned rate = c.total_us ? (unsigned)(c.total_bytes * 1000000 / c.total_us / 1024) : 0;
        printf("%lu file%s, %lu KB, %u KB/s%s\n", (unsigned long)c.files, c.files == 1 ? "" : "s",
               (unsigned long)((c.total_bytes + 1023) / 1024), rate, ok ? "" : " (then it failed)");
    }
}
