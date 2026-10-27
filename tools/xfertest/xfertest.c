// Runs src/xfer/xfer.c on a PC against another program (lrzsz: sz, rz, sx,
// rx, sb, rb) connected through a socket pair, to test the protocols.
//
//   xfertest send x|1|y|z FILE... -- COMMAND ARGS...   (we send, it receives)
//   xfertest recv x|1|y|z [NAME] -- COMMAND ARGS...    (it sends, we receive)
//
// Received files go into the current folder. Options before send/recv:
//   -cancel N   act as if the user pressed Esc after N bytes
//   -cancel-ms N  ... after N milliseconds
//   -noise N    flip a bit in every Nth byte we read (line noise)
// Build: cc -O2 -I../../src/xfer xfertest.c ../../src/xfer/xfer.c -o xfertest

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "xfer.h"

static int fd;
static long cancel_after = -1, cancel_ms = -1, noise_every = 0, total_in;
static struct timespec started;
static uint8_t inbuf[4096];
static int in_len, in_pos;

static int t_getc(void *ctx, unsigned timeout_ms) {
    if (cancel_after >= 0 && total_in >= cancel_after)
        return XFER_ABORT;
    if (cancel_ms >= 0) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if ((now.tv_sec - started.tv_sec) * 1000 + (now.tv_nsec - started.tv_nsec) / 1000000 >= cancel_ms)
            return XFER_ABORT;
    }
    if (in_pos == in_len) {
        struct pollfd p = {fd, POLLIN, 0};
        if (poll(&p, 1, timeout_ms) <= 0)
            return XFER_TIMEOUT;
        int n = read(fd, inbuf, sizeof inbuf);
        if (n <= 0) {
            usleep(timeout_ms * 1000);  // the other end has gone
            return XFER_TIMEOUT;
        }
        in_len = n;
        in_pos = 0;
    }
    ++total_in;
    int c = inbuf[in_pos++];
    if (noise_every && total_in % noise_every == 0)
        c ^= 0x04;
    return c;
}

static void t_write(void *ctx, const uint8_t *data, int len) {
    while (len > 0) {
        int n = write(fd, data, len);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return;
        }
        data += n;
        len -= n;
    }
}

static void *t_open_read(void *ctx, const char *path, uint32_t *size, char *name, int name_size) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    struct stat st;
    fstat(fileno(f), &st);
    *size = st.st_size;
    const char *b = strrchr(path, '/');
    snprintf(name, name_size, "%s", b ? b + 1 : path);
    return f;
}

static void *t_open_write(void *ctx, const char *name, uint32_t size, bool resume, uint32_t *offset,
                          char *saved_as, int saved_as_size) {
    struct stat st;
    *offset = 0;
    if (resume && stat(name, &st) == 0 && (uint32_t)st.st_size < size) {
        *offset = st.st_size;
        snprintf(saved_as, saved_as_size, "%s", name);
        return fopen(name, "ab");
    }
    char try[300];
    snprintf(try, sizeof try, "%s", name);
    for (int i = 1; stat(try, &st) == 0 && i < 100; ++i)
        snprintf(try, sizeof try, "%s-%d", name, i);
    snprintf(saved_as, saved_as_size, "%s", try);
    return fopen(try, "wb");
}

static int t_read(void *ctx, void *f, uint8_t *buf, int len) {
    return fread(buf, 1, len, f);
}

static bool t_seek(void *ctx, void *f, uint32_t off) {
    return fseek(f, off, SEEK_SET) == 0;
}

static bool t_write_file(void *ctx, void *f, const uint8_t *buf, int len) {
    return fwrite(buf, 1, len, f) == (size_t)len;
}

static void t_close(void *ctx, void *f, bool ok) {
    fclose(f);
}

static void t_progress(void *ctx, const xfer_progress_t *p) {
    static time_t last;
    if (time(NULL) != last) {
        last = time(NULL);
        fprintf(stderr, "  %s %u/%u errors %d\n", p->name, p->done, p->size, p->errors);
    }
}

int main(int argc, char **argv) {
    int i = 1;
    for (; i < argc && argv[i][0] == '-'; i += 2) {
        if (!strcmp(argv[i], "-cancel"))
            cancel_after = atol(argv[i + 1]);
        else if (!strcmp(argv[i], "-cancel-ms"))
            cancel_ms = atol(argv[i + 1]);
        else if (!strcmp(argv[i], "-noise"))
            noise_every = atol(argv[i + 1]);
    }
    if (argc - i < 4)
        return fprintf(stderr, "usage: see xfertest.c\n"), 2;
    bool sending = !strcmp(argv[i], "send");
    char p = argv[i + 1][0];
    xfer_proto_t proto = p == 'x' ? XFER_XMODEM : p == '1' ? XFER_XMODEM_1K
                         : p == 'y' ? XFER_YMODEM : XFER_ZMODEM;
    const char *files[64];
    int nfiles = 0, j = i + 2;
    for (; j < argc && strcmp(argv[j], "--"); ++j)
        files[nfiles++] = argv[j];
    if (j >= argc - 1)
        return fprintf(stderr, "no command after --\n"), 2;
    int sv[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    signal(SIGPIPE, SIG_IGN);
    pid_t pid = fork();
    if (pid == 0) {
        dup2(sv[1], 0);
        dup2(sv[1], 1);
        close(sv[0]);
        close(sv[1]);
        execvp(argv[j + 1], argv + j + 1);
        _exit(127);
    }
    close(sv[1]);
    fd = sv[0];
    xfer_io_t io = {NULL,   t_getc, t_write,      t_open_read, t_open_write,
                    t_read, t_seek, t_write_file, t_close,     t_progress};
    xfer_result_t r;
    clock_gettime(CLOCK_MONOTONIC, &started);
    if (sending)
        xfer_send(&io, proto, files, nfiles, &r);
    else
        xfer_receive(&io, proto, nfiles ? files[0] : "XMODEM.BIN", &r);
    printf("%s: %s %s, %d files, %u bytes: %s\n", xfer_proto_name(proto), sending ? "send" : "receive",
           r.ok ? "OK" : "FAILED", r.files, r.bytes, r.message);
    // Give the other program a moment, then end it.
    for (int k = 0; k < 50; ++k) {
        if (waitpid(pid, NULL, WNOHANG) == pid)
            return r.ok ? 0 : 1;
        usleep(100000);
    }
    kill(pid, SIGTERM);
    waitpid(pid, NULL, 0);
    return r.ok ? 0 : 1;
}
