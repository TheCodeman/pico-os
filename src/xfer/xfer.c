/**
 * @file xfer.c
 * @brief XMODEM, YMODEM and ZMODEM, after Chuck Forsberg's "XMODEM/YMODEM Protocol Reference" and "The ZMODEM Inter Application File Transfer Protocol".
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */

#include "xfer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SOH 0x01  ///< start of a 128-byte block
#define STX 0x02  ///< start of a 1024-byte block
#define EOT 0x04  ///< end of the file
#define ACK 0x06  ///< acknowledged
#define BS  0x08  ///< backspace
#define NAK 0x15  ///< not acknowledged
#define CAN 0x18  ///< cancel
#define SUB 0x1a  ///< the padding in a last block
#define XON 0x11  ///< XON

/// How often a block is tried.
#define RETRIES 10

/// Results of the reading helpers besides a byte (all negative).
#define R_TIMEOUT XFER_TIMEOUT
#define R_ABORT   XFER_ABORT  ///< the user cancelled
#define R_CANCEL  (-3)        ///< the other end cancelled
#define R_BAD     (-4)        ///< garbled: CRC or framing error

/// A transfer's state.
typedef struct {
    const xfer_io_t *io;  ///< the callbacks
    xfer_progress_t prog;  ///< the progress shown
    xfer_result_t *res;  ///< the result
    char name[64];  ///< the file's name
    /// ZMODEM
    bool rx_crc32;      ///< the header just read was ZBIN32: its data uses CRC-32
    uint8_t last_sent;  ///< for escaping CR after @
    uint8_t out[1100];  ///< bytes to send, gathered
    int out_len;  ///< their count
} xs_t;

const char *xfer_proto_name(xfer_proto_t proto) {
    switch (proto) {
    case XFER_XMODEM: return "XMODEM";
    case XFER_XMODEM_1K: return "XMODEM-1K";
    case XFER_YMODEM: return "YMODEM";
    case XFER_ZMODEM: return "ZMODEM";
    }
    return "?";
}

// ---- Shared ---------------------------------------------------------------------

/// XMODEM's CRC-16 (CCITT).
/// @param crc the CRC so far
/// @param p the bytes
/// @param n their count
/// @return the CRC
static uint16_t crc16(uint16_t crc, const uint8_t *p, int n) {
    while (n--) {
        crc ^= (uint16_t)*p++ << 8;
        for (int i = 0; i < 8; ++i)
            crc = crc & 0x8000 ? (uint16_t)(crc << 1) ^ 0x1021 : (uint16_t)(crc << 1);
    }
    return crc;
}

/// ZMODEM's CRC-32.
/// @param crc the CRC so far
/// @param p the bytes
/// @param n their count
/// @return the CRC
static uint32_t crc32(uint32_t crc, const uint8_t *p, int n) {
    while (n--) {
        crc ^= *p++;
        for (int i = 0; i < 8; ++i)
            crc = crc & 1 ? (crc >> 1) ^ 0xedb88320u : crc >> 1;
    }
    return crc;
}

/// A byte from the line.
/// @param s the transfer
/// @param ms how long to wait
/// @return the byte, or a negative R_ value
static int rd(xs_t *s, unsigned ms) {
    return s->io->getc(s->io->ctx, ms);
}

/// Sends bytes.
/// @param s the transfer
/// @param p the bytes
/// @param n their count
static void wr(xs_t *s, const void *p, int n) {
    s->io->write(s->io->ctx, p, n);
}

/// Sends a byte.
/// @param s the transfer
/// @param c it
static void wr1(xs_t *s, uint8_t c) {
    wr(s, &c, 1);
}

/// Reads and drops whatever arrives until the line is quiet for `ms`.
/// @param s the transfer
/// @param ms the quiet time
/// @return 0, or a negative R_ value
static int purge(xs_t *s, unsigned ms) {
    int c;
    while ((c = rd(s, ms)) >= 0)
        ;
    return c == R_ABORT ? R_ABORT : 0;
}

/// Reports the progress.
/// @param s the transfer
static void progress(xs_t *s) {
    if (s->io->progress)
        s->io->progress(s->io->ctx, &s->prog);
}

/// Sets the result.
/// @param s the transfer
/// @param ok it worked
/// @param fmt the message, printf's format with one %s
/// @param arg its argument
static void say(xs_t *s, bool ok, const char *fmt, const char *arg) {
    s->res->ok = ok;
    snprintf(s->res->message, sizeof s->res->message, fmt, arg);
}

/// Tells the other end to stop: CANs (five or more stop all three
/// protocols), then backspaces to wipe them from a terminal.
/// @param s the transfer
static void send_cancel(xs_t *s) {
    static const uint8_t seq[] = {CAN, CAN, CAN, CAN, CAN, CAN, CAN, CAN,
                                  BS,  BS,  BS,  BS,  BS,  BS,  BS,  BS};
    wr(s, seq, sizeof seq);
}

/// Ends a transfer that was stopped.
/// @param s the transfer
/// @param why R_CANCEL, R_ABORT or R_TIMEOUT
static void stopped(xs_t *s, int why) {
    if (why == R_CANCEL) {
        say(s, false, "The other end cancelled.", NULL);
    } else {
        send_cancel(s);
        if (why == R_ABORT)
            say(s, false, "Cancelled.", NULL);
        else if (why == R_TIMEOUT)
            say(s, false, "No answer from the other end.", NULL);
    }
}

/// The last part of a path: what is sent as the name.
/// @param path the path
/// @return its last part
static const char *base_name(const char *path) {
    const char *b = path;
    for (const char *p = path; *p; ++p)
        if (*p == '/' || *p == '\\' || *p == ':')
            b = p + 1;
    return b;
}

/// A received name made safe: no folders, no leading dots, no control
/// characters.
/// @param in the name
/// @param out the safe name
/// @param size room in out
static void clean_name(const char *in, char *out, int size) {
    const char *b = base_name(in);
    while (*b == '.')
        ++b;
    int n = 0;
    for (; *b && n < size - 1; ++b)
        out[n++] = (unsigned char)*b < 0x20 || strchr("\"*<>?|", *b) ? '_' : *b;
    out[n] = 0;
    if (!n)
        snprintf(out, size, "RECEIVED.BIN");
}

/// Counts a file done.
/// @param s the transfer
/// @param bytes its size
static void file_done(xs_t *s, uint32_t bytes) {
    ++s->res->files;
    s->res->bytes += bytes;
    ++s->prog.files;
}

// ---- XMODEM and YMODEM ------------------------------------------------------------

/// Reads one block. Returns its length (128 or 1024) with its number in
/// *seq, 0 for EOT, or a negative R_ value.
/// @param s the transfer
/// @param crc CRC-16 (else a checksum)
/// @param buf where
/// @param seq set to its number
/// @param timeout_ms how long to wait
/// @return its length, 0 for EOT, or a negative R_ value
static int read_block(xs_t *s, bool crc, uint8_t *buf, int *seq, unsigned timeout_ms) {
    int c = rd(s, timeout_ms);
    if (c < 0)
        return c;
    if (c == EOT)
        return 0;
    if (c == CAN) {
        c = rd(s, 1000);
        return c == CAN ? R_CANCEL : c == R_ABORT ? R_ABORT : R_BAD;
    }
    int len = c == SOH ? 128 : c == STX ? 1024 : 0;
    if (!len)
        return R_BAD;
    int b = rd(s, 1000), nb = rd(s, 1000);
    if (b < 0 || nb < 0)
        return b == R_ABORT || nb == R_ABORT ? R_ABORT : R_BAD;
    for (int i = 0; i < len; ++i) {
        c = rd(s, 1000);
        if (c < 0)
            return c == R_ABORT ? R_ABORT : R_BAD;
        buf[i] = c;
    }
    if (crc) {
        int hi = rd(s, 1000), lo = rd(s, 1000);
        if (hi < 0 || lo < 0)
            return hi == R_ABORT || lo == R_ABORT ? R_ABORT : R_BAD;
        if (crc16(0, buf, len) != (uint16_t)(hi << 8 | lo))
            return R_BAD;
    } else {
        int sum = rd(s, 1000);
        if (sum < 0)
            return sum == R_ABORT ? R_ABORT : R_BAD;
        uint8_t total = 0;
        for (int i = 0; i < len; ++i)
            total += buf[i];
        if (total != sum)
            return R_BAD;
    }
    if ((b ^ nb) != 0xff)
        return R_BAD;
    *seq = b;
    return len;
}

/// Receives by XMODEM or YMODEM.
/// @param s the transfer
/// @param ymodem YMODEM (file names and sizes in block 0)
/// @param xname XMODEM: the file's name
static void xy_receive(xs_t *s, bool ymodem, const char *xname) {
    uint8_t *buf = malloc(1024);
    if (!buf) {
        say(s, false, "Out of memory.", NULL);
        return;
    }
    bool crc = true;
    for (;;) {  // each file of a YMODEM batch
        int len = R_TIMEOUT, seq = 0;
        // Ask for the first block: 'C' for CRC-16; XMODEM falls back to a
        // NAK (checksums) if 'C' gets no answer.
        for (int tries = 0;; ++tries) {
            if (tries == (ymodem ? 2 * RETRIES : 2 * RETRIES)) {
                stopped(s, R_TIMEOUT);
                goto out;
            }
            if (!ymodem && tries == 4)
                crc = false;
            wr1(s, crc ? 'C' : NAK);
            len = read_block(s, crc, buf, &seq, 3000);
            if (len == R_ABORT || len == R_CANCEL) {
                stopped(s, len);
                goto out;
            }
            if (len > 0 || (len == 0 && !ymodem))
                break;
            if (len == R_BAD && purge(s, 500) == R_ABORT) {
                stopped(s, R_ABORT);
                goto out;
            }
        }
        void *f = NULL;
        uint32_t size = 0, got = 0, offset = 0;
        char saved[64];
        unsigned expect = 1;
        bool first = true;  // no data block yet: a timeout asks again with 'C'
        if (ymodem) {
            if (seq != 0) {
                stopped(s, R_TIMEOUT);
                say(s, false, "The sender didn't start with a YMODEM header.", NULL);
                goto out;
            }
            if (!buf[0]) {  // an empty name ends the batch
                wr1(s, ACK);
                s->res->ok = true;
                break;
            }
            char name[64];
            clean_name((char *)buf, name, sizeof name);
            const char *p = (char *)buf + strlen((char *)buf) + 1;
            size = strtoul(p, NULL, 10);
            f = s->io->open_write(s->io->ctx, name, size, false, &offset, saved, sizeof saved);
            if (!f) {
                stopped(s, R_TIMEOUT);
                say(s, false, "Can't create %s.", name);
                goto out;
            }
            wr1(s, ACK);
            wr1(s, 'C');
            len = read_block(s, crc, buf, &seq, 10000);
        } else {
            f = s->io->open_write(s->io->ctx, xname, 0, false, &offset, saved, sizeof saved);
            if (!f) {
                stopped(s, R_TIMEOUT);
                say(s, false, "Can't create %s.", xname);
                goto out;
            }
        }
        snprintf(s->name, sizeof s->name, "%s", saved);
        s->prog = (xfer_progress_t){s->name, 0, size, 0, s->prog.files};
        progress(s);
        int errors = 0;
        for (;;) {
            if (len == 0) {  // EOT
                wr1(s, ACK);
                s->io->close(s->io->ctx, f, true);
                file_done(s, got);
                break;
            }
            if (len < 0) {
                if (len == R_ABORT || len == R_CANCEL || ++errors > RETRIES) {
                    s->io->close(s->io->ctx, f, false);
                    stopped(s, len == R_ABORT || len == R_CANCEL ? len : R_TIMEOUT);
                    if (len != R_ABORT && len != R_CANCEL)
                        say(s, false, "Too many errors.", NULL);
                    goto out;
                }
                ++s->prog.errors;
                if (purge(s, 500) == R_ABORT) {
                    len = R_ABORT;
                    continue;
                }
                wr1(s, first ? (crc ? 'C' : NAK) : NAK);
            } else if (seq == (int)((expect - 1) & 0xff)) {
                wr1(s, ACK);  // a repeat: our ACK was lost
            } else if (seq != (int)(expect & 0xff)) {
                s->io->close(s->io->ctx, f, false);
                stopped(s, R_TIMEOUT);
                say(s, false, "Blocks out of order.", NULL);
                goto out;
            } else {
                int n = len;
                if (size && got + n > size)
                    n = size - got;  // YMODEM: the last block's padding
                if (n > 0 && !s->io->write_file(s->io->ctx, f, buf, n)) {
                    s->io->close(s->io->ctx, f, false);
                    stopped(s, R_TIMEOUT);
                    say(s, false, "Can't write %s (drive full?).", s->name);
                    goto out;
                }
                got += n;
                ++expect;
                first = false;
                errors = 0;
                wr1(s, ACK);
                s->prog.done = got;
                progress(s);
            }
            len = read_block(s, crc, buf, &seq, 10000);
        }
        if (!ymodem) {
            s->res->ok = true;
            break;
        }
    }
out:
    free(buf);
    if (s->res->ok)
        snprintf(s->res->message, sizeof s->res->message, "Received %d file%s, %lu bytes.",
                 s->res->files, s->res->files == 1 ? "" : "s", (unsigned long)s->res->bytes);
}

/// Waits for the receiver's go-ahead: 'C' (CRC) or NAK (checksum). Returns
/// the character or a negative R_ value.
/// @param s the transfer
/// @param timeout_ms how long to wait
/// @return 'C', NAK, or a negative R_ value
static int wait_start(xs_t *s, unsigned timeout_ms) {
    unsigned waited = 0;
    while (waited < timeout_ms) {
        int c = rd(s, 1000);
        if (c == R_TIMEOUT) {
            waited += 1000;
            continue;
        }
        if (c < 0)
            return c;
        if (c == 'C' || c == NAK)
            return c;
        if (c == CAN && rd(s, 1000) == CAN)
            return R_CANCEL;
    }
    return R_TIMEOUT;
}

/// Waits for ACK (1), NAK (0) or a negative R_ value.
/// @param s the transfer
/// @return 1, 0 or a negative R_ value
static int wait_ack(xs_t *s) {
    for (;;) {
        int c = rd(s, 10000);
        if (c < 0)
            return c;
        if (c == ACK)
            return 1;
        if (c == NAK)
            return 0;
        if (c == CAN && rd(s, 1000) == CAN)
            return R_CANCEL;
        // anything else ('C' repeated, noise): keep waiting
    }
}

/// Sends one block until it is ACKed. 1 sent, else a negative R_ value.
/// @param s the transfer
/// @param seq its number
/// @param data its bytes
/// @param len 128 or 1024
/// @param crc CRC-16 (else a checksum)
/// @return 1, or a negative R_ value
static int send_block(xs_t *s, int seq, const uint8_t *data, int len, bool crc) {
    uint8_t head[3] = {len == 1024 ? STX : SOH, (uint8_t)seq, (uint8_t)~seq};
    uint8_t tail[2];
    int tail_len;
    if (crc) {
        uint16_t c = crc16(0, data, len);
        tail[0] = c >> 8;
        tail[1] = c;
        tail_len = 2;
    } else {
        uint8_t sum = 0;
        for (int i = 0; i < len; ++i)
            sum += data[i];
        tail[0] = sum;
        tail_len = 1;
    }
    for (int tries = 0; tries < RETRIES; ++tries) {
        wr(s, head, 3);
        wr(s, data, len);
        wr(s, tail, tail_len);
        int r = wait_ack(s);
        if (r == 1)
            return 1;
        if (r == R_ABORT || r == R_CANCEL)
            return r;
        ++s->prog.errors;
        progress(s);
    }
    return R_TIMEOUT;
}

/// Sends EOT until it is acknowledged.
/// @param s the transfer
/// @return 1, or a negative R_ value
static int send_eot(xs_t *s) {
    for (int tries = 0; tries < RETRIES; ++tries) {
        wr1(s, EOT);
        int r = wait_ack(s);
        if (r == 1)
            return 1;
        if (r == R_ABORT || r == R_CANCEL)
            return r;
    }
    return R_TIMEOUT;
}

/// Sends by XMODEM, XMODEM-1K or YMODEM.
/// @param s the transfer
/// @param proto the protocol
/// @param paths the files
/// @param count their count
static void xy_send(xs_t *s, xfer_proto_t proto, const char *const *paths, int count) {
    bool ymodem = proto == XFER_YMODEM;
    uint8_t *buf = malloc(1024);
    if (!buf) {
        say(s, false, "Out of memory.", NULL);
        return;
    }
    int r = wait_start(s, 60000);
    if (r < 0) {
        stopped(s, r);
        goto out;
    }
    bool crc = r == 'C';
    if (!ymodem)
        count = count > 1 ? 1 : count;
    for (int i = 0; i < count; ++i) {
        uint32_t size;
        void *f = s->io->open_read(s->io->ctx, paths[i], &size, s->name, sizeof s->name);
        if (!f) {
            stopped(s, R_TIMEOUT);
            say(s, false, "Can't open %s.", paths[i]);
            goto out;
        }
        s->prog = (xfer_progress_t){s->name, 0, size, s->prog.errors, s->prog.files};
        progress(s);
        if (ymodem) {
            if (i > 0 && (r = wait_start(s, 60000)) < 0) {
                s->io->close(s->io->ctx, f, false);
                stopped(s, r);
                goto out;
            }
            memset(buf, 0, 128);
            int n = snprintf((char *)buf, 100, "%s", s->name) + 1;
            snprintf((char *)buf + n, 128 - n, "%lu", (unsigned long)size);
            if ((r = send_block(s, 0, buf, 128, true)) < 0 || (r = wait_start(s, 10000)) < 0) {
                s->io->close(s->io->ctx, f, false);
                stopped(s, r);
                goto out;
            }
        }
        bool big = proto != XFER_XMODEM && crc;
        unsigned seq = 1;
        uint32_t sent = 0;
        for (;;) {
            int want = big && size - sent > 128 ? 1024 : 128;
            int n = s->io->read(s->io->ctx, f, buf, want);
            if (n < 0) {
                s->io->close(s->io->ctx, f, false);
                stopped(s, R_TIMEOUT);
                say(s, false, "Can't read %s.", s->name);
                goto out;
            }
            if (n == 0)
                break;
            int len = n > 128 ? 1024 : 128;
            memset(buf + n, SUB, len - n);
            if ((r = send_block(s, seq & 0xff, buf, len, crc)) < 0) {
                s->io->close(s->io->ctx, f, false);
                stopped(s, r);
                if (r == R_TIMEOUT)
                    say(s, false, "Too many errors.", NULL);
                goto out;
            }
            ++seq;
            sent += n;
            s->prog.done = sent;
            progress(s);
        }
        s->io->close(s->io->ctx, f, true);
        if ((r = send_eot(s)) < 0) {
            stopped(s, r);
            goto out;
        }
        file_done(s, sent);
    }
    if (ymodem) {  // an empty header ends the batch
        if ((r = wait_start(s, 10000)) < 0 ||
            (memset(buf, 0, 128), r = send_block(s, 0, buf, 128, true)) < 0) {
            stopped(s, r);
            goto out;
        }
    }
    s->res->ok = true;
    snprintf(s->res->message, sizeof s->res->message, "Sent %d file%s, %lu bytes.", s->res->files,
             s->res->files == 1 ? "" : "s", (unsigned long)s->res->bytes);
out:
    free(buf);
}

// ---- ZMODEM -------------------------------------------------------------------------

#define ZPAD   '*'  ///< frame start padding
#define ZDLE   0x18  ///< the escape character
#define ZBIN   'A'  ///< binary header, CRC-16
#define ZHEX   'B'  ///< hex header
#define ZBIN32 'C'  ///< binary header, CRC-32

/// ZMODEM frame types.
enum {
    ZRQINIT, ZRINIT, ZSINIT, ZACK, ZFILE, ZSKIP, ZNAK, ZABORT, ZFIN, ZRPOS, ZDATA, ZEOF,
    ZFERR, ZCRC, ZCHALLENGE, ZCOMPL, ZCAN, ZFREECNT, ZCOMMAND, ZSTDERR,
};

#define ZCRCE 'h'  ///< data subpacket ends, frame ends, header follows
#define ZCRCG 'i'  ///< ... frame goes on
#define ZCRCQ 'j'  ///< ... frame goes on, ZACK wanted
#define ZCRCW 'k'  ///< ... frame ends, ZACK wanted
#define ZRUB0 'l'  ///< an escaped 0x7f
#define ZRUB1 'm'  ///< an escaped 0xff

/// Header bytes: positions P0 (low) .. P3, or flags F3 .. F0.
#define ZF0 3
#define ZF1 2  ///< flag byte F1

#define CANFDX  0x01  ///< ZRINIT flag: full duplex
#define CANOVIO 0x02  ///< ZRINIT flag: can receive while writing
#define CANFC32 0x20  ///< ZRINIT flag: takes CRC-32
#define ZCBIN   1  ///< ZFILE conversion: binary
#define ZCRESUM 3  ///< ZFILE conversion: resume

#define Z_BUFLEN  4096  ///< we ask the sender to wait for an ACK this often
#define Z_SUBPACK 1024  ///< data subpacket size

/// A header's position (P0-P3).
/// @param h the header's bytes
/// @return the position
static uint32_t hdr_pos(const uint8_t h[4]) {
    return h[0] | (uint32_t)h[1] << 8 | (uint32_t)h[2] << 16 | (uint32_t)h[3] << 24;
}

/// A position into a header.
/// @param h the header's bytes
/// @param pos the position
static void pos_hdr(uint8_t h[4], uint32_t pos) {
    h[0] = pos;
    h[1] = pos >> 8;
    h[2] = pos >> 16;
    h[3] = pos >> 24;
}

/// Sends what was gathered.
/// @param s the transfer
static void zflush(xs_t *s) {
    if (s->out_len)
        wr(s, s->out, s->out_len);
    s->out_len = 0;
}

/// Gathers a byte to send.
/// @param s the transfer
/// @param c the byte
static void zraw(xs_t *s, uint8_t c) {
    if (s->out_len == sizeof s->out)
        zflush(s);
    s->out[s->out_len++] = c;
    s->last_sent = c;
}

/// One byte with ZDLE escaping: ZDLE, the flow control characters and CR
/// after @ (which some networks take as a command).
/// @param s the transfer
/// @param c the byte
static void zesc(xs_t *s, uint8_t c) {
    uint8_t low = c & 0x7f;
    if (c == ZDLE || low == 0x10 || low == 0x11 || low == 0x13 ||
        (low == 0x0d && (s->last_sent & 0x7f) == '@')) {
        zraw(s, ZDLE);
        zraw(s, c ^ 0x40);
    } else {
        zraw(s, c);
    }
}

/// A byte as two hex digits.
/// @param s the transfer
/// @param c the byte
static void zhex_byte(xs_t *s, uint8_t c) {
    static const char hex[] = "0123456789abcdef";
    zraw(s, hex[c >> 4]);
    zraw(s, hex[c & 15]);
}

/// Sends a hex header.
/// @param s the transfer
/// @param type its type
/// @param h its 4 bytes
static void zsend_hex_header(xs_t *s, int type, const uint8_t h[4]) {
    uint8_t b[5] = {(uint8_t)type, h[0], h[1], h[2], h[3]};
    zraw(s, ZPAD);
    zraw(s, ZPAD);
    zraw(s, ZDLE);
    zraw(s, ZHEX);
    for (int i = 0; i < 5; ++i)
        zhex_byte(s, b[i]);
    uint16_t c = crc16(0, b, 5);
    zhex_byte(s, c >> 8);
    zhex_byte(s, c);
    zraw(s, '\r');
    zraw(s, 0x8a);
    if (type != ZFIN && type != ZACK)
        zraw(s, XON);
    zflush(s);
}

/// Sends a binary header (CRC-16).
/// @param s the transfer
/// @param type its type
/// @param h its 4 bytes
static void zsend_bin_header(xs_t *s, int type, const uint8_t h[4]) {
    uint8_t b[5] = {(uint8_t)type, h[0], h[1], h[2], h[3]};
    zraw(s, ZPAD);
    zraw(s, ZDLE);
    zraw(s, ZBIN);
    for (int i = 0; i < 5; ++i)
        zesc(s, b[i]);
    uint16_t c = crc16(0, b, 5);
    zesc(s, c >> 8);
    zesc(s, c);
    zflush(s);
}

/// Sends a data subpacket.
/// @param s the transfer
/// @param buf the data
/// @param len its length
/// @param end its frame end (ZCRCE..ZCRCW)
static void zsend_data(xs_t *s, const uint8_t *buf, int len, uint8_t end) {
    for (int i = 0; i < len; ++i)
        zesc(s, buf[i]);
    zraw(s, ZDLE);
    zraw(s, end);
    uint16_t c = crc16(crc16(0, buf, len), &end, 1);
    zesc(s, c >> 8);
    zesc(s, c);
    if (end == ZCRCW)
        zraw(s, XON);
    zflush(s);
}

/// The next byte of an escaped stream: 0-255, a frame end (ZCRCx | 0x100),
/// or a negative R_ value.
/// @param s the transfer
/// @param ms how long to wait
/// @return the byte, frame end or R_ value
static int zdl_read(xs_t *s, unsigned ms) {
    int c;
    for (;;) {
        c = rd(s, ms);
        if (c < 0)
            return c;
        if (c == ZDLE)
            break;
        if (c == 0x11 || c == 0x13 || c == 0x91 || c == 0x93)
            continue;  // flow control, never data (data has them escaped)
        return c;
    }
    int cans = 1;
    for (;;) {
        c = rd(s, ms);
        if (c < 0)
            return c;
        switch (c) {
        case ZDLE:
            if (++cans >= 5)
                return R_CANCEL;
            continue;
        case ZCRCE:
        case ZCRCG:
        case ZCRCQ:
        case ZCRCW: return c | 0x100;
        case ZRUB0: return 0x7f;
        case ZRUB1: return 0xff;
        case 0x11:
        case 0x13:
        case 0x91:
        case 0x93: continue;
        default:
            if ((c & 0x60) == 0x40)
                return c ^ 0x40;
            return R_BAD;
        }
    }
}

/// A hex digit's value.
/// @param c the character
/// @return 0-15, or -1
static int hex_digit(int c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

/// Waits for a header (skipping anything else). Its type, or a negative R_
/// value (R_BAD for a damaged one).
/// @param s the transfer
/// @param h set to its 4 bytes
/// @param timeout_ms how long to wait
/// @return its type, or a negative R_ value
static int zget_header(xs_t *s, uint8_t h[4], unsigned timeout_ms) {
    int cans = 0;
    for (;;) {
        int c = rd(s, timeout_ms);
        if (c < 0)
            return c;
        if (c == CAN) {
            if (++cans >= 5)
                return R_CANCEL;
            continue;
        }
        cans = 0;
        if (c != ZPAD)
            continue;
        do
            c = rd(s, 1000);
        while (c == ZPAD);
        if (c < 0)
            return c;
        if (c != ZDLE)
            continue;
        int kind = rd(s, 1000);
        if (kind < 0)
            return kind;
        uint8_t b[9];
        if (kind == ZHEX) {
            for (int i = 0; i < 7; ++i) {
                int hi = rd(s, 1000), lo = rd(s, 1000);
                if (hi < 0 || lo < 0)
                    return hi == R_ABORT || lo == R_ABORT ? R_ABORT : R_BAD;
                if (hex_digit(hi) < 0 || hex_digit(lo) < 0)
                    return R_BAD;
                b[i] = hex_digit(hi) << 4 | hex_digit(lo);
            }
            if (crc16(0, b, 5) != (uint16_t)(b[5] << 8 | b[6]))
                return R_BAD;
            s->rx_crc32 = false;
        } else if (kind == ZBIN || kind == ZBIN32) {
            int n = kind == ZBIN ? 7 : 9;
            for (int i = 0; i < n; ++i) {
                c = zdl_read(s, 1000);
                if (c < 0 || c > 0xff)
                    return c == R_ABORT || c == R_CANCEL ? c : R_BAD;
                b[i] = c;
            }
            if (kind == ZBIN) {
                if (crc16(0, b, 5) != (uint16_t)(b[5] << 8 | b[6]))
                    return R_BAD;
            } else {
                uint32_t want = ~crc32(0xffffffffu, b, 5);
                if (want != hdr_pos(b + 5))
                    return R_BAD;
            }
            s->rx_crc32 = kind == ZBIN32;
        } else {
            continue;
        }
        memcpy(h, b + 1, 4);
        return b[0];
    }
}

/// Reads a data subpacket into buf. Its frame end (ZCRCE..ZCRCW) with the
/// length in *len, or a negative R_ value.
/// @param s the transfer
/// @param buf where
/// @param max room in buf
/// @param len set to the length
/// @return the frame end, or a negative R_ value
static int zget_data(xs_t *s, uint8_t *buf, int max, int *len) {
    int n = 0;
    for (;;) {
        int c = zdl_read(s, 10000);
        if (c < 0)
            return c;
        if (c & 0x100) {
            uint8_t end = c & 0xff;
            int ncrc = s->rx_crc32 ? 4 : 2;
            uint8_t cb[4];
            for (int i = 0; i < ncrc; ++i) {
                c = zdl_read(s, 1000);
                if (c < 0 || c > 0xff)
                    return c == R_ABORT || c == R_CANCEL ? c : R_BAD;
                cb[i] = c;
            }
            if (s->rx_crc32) {
                uint32_t crc = ~crc32(crc32(0xffffffffu, buf, n), &end, 1);
                if (crc != hdr_pos(cb))
                    return R_BAD;
            } else {
                uint16_t crc = crc16(crc16(0, buf, n), &end, 1);
                if (crc != (uint16_t)(cb[0] << 8 | cb[1]))
                    return R_BAD;
            }
            *len = n;
            return end;
        }
        if (n >= max)
            return R_BAD;
        buf[n++] = c;
    }
}

/// Sends a header with a position.
/// @param s the transfer
/// @param type its type
/// @param pos the position
static void zsend_pos(xs_t *s, int type, uint32_t pos) {
    uint8_t h[4];
    pos_hdr(h, pos);
    zsend_hex_header(s, type, h);
}

/// Sends ZRINIT: our buffer and abilities.
/// @param s the transfer
static void zsend_rinit(xs_t *s) {
    uint8_t h[4] = {Z_BUFLEN & 0xff, Z_BUFLEN >> 8, 0, CANFDX | CANFC32};
    zsend_hex_header(s, ZRINIT, h);
}

/// Ends a ZMODEM session we cancelled or that failed.
/// @param s the transfer
/// @param why the R_ value
/// @param msg the message, or NULL
/// @param arg its argument
static void zabort(xs_t *s, int why, const char *msg, const char *arg) {
    stopped(s, why == R_CANCEL || why == R_ABORT ? why : R_TIMEOUT);
    if (msg)
        say(s, false, msg, arg);
}

/// Receives by ZMODEM.
/// @param s the transfer
static void z_receive(xs_t *s) {
    uint8_t *buf = malloc(Z_SUBPACK + 16);
    uint8_t *wbuf = malloc(Z_BUFLEN + Z_SUBPACK);
    if (!buf || !wbuf) {
        free(buf);
        free(wbuf);
        say(s, false, "Out of memory.", NULL);
        return;
    }
    void *f = NULL;
    uint32_t offset = 0, start = 0;
    int wlen = 0, timeouts = 0, errors = 0;
    bool done = false;
    zsend_rinit(s);
    while (!done) {
        uint8_t h[4];
        int type = zget_header(s, h, 10000);
        if (type == R_TIMEOUT || type == R_BAD) {
            if (++timeouts > RETRIES) {
                zabort(s, R_TIMEOUT, type == R_BAD ? "Too many errors." : NULL, NULL);
                break;
            }
            ++s->prog.errors;
            if (f)
                zsend_pos(s, ZRPOS, offset);
            else
                zsend_rinit(s);
            continue;
        }
        if (type < 0) {
            zabort(s, type, NULL, NULL);
            break;
        }
        timeouts = 0;
        switch (type) {
        case ZRQINIT: zsend_rinit(s); break;
        case ZSINIT: {
            int len;
            if (zget_data(s, buf, Z_SUBPACK, &len) >= 0)
                zsend_pos(s, ZACK, 1);
            else
                zsend_hex_header(s, ZNAK, (uint8_t[4]){0});
            break;
        }
        case ZFILE: {
            int len;
            int end = zget_data(s, buf, Z_SUBPACK, &len);
            if (end < 0) {
                if (end == R_ABORT || end == R_CANCEL) {
                    zabort(s, end, NULL, NULL);
                    done = true;
                    break;
                }
                zsend_hex_header(s, ZNAK, (uint8_t[4]){0});
                break;
            }
            buf[len < Z_SUBPACK ? len : Z_SUBPACK - 1] = 0;
            char name[64], saved[64];
            clean_name((char *)buf, name, sizeof name);
            const char *p = (char *)buf + strlen((char *)buf) + 1;
            uint32_t size = (const uint8_t *)p < buf + len ? strtoul(p, NULL, 10) : 0;
            if (f) {  // a repeated ZFILE while one is open: start again
                s->io->close(s->io->ctx, f, false);
                f = NULL;
            }
            f = s->io->open_write(s->io->ctx, name, size, h[ZF0] == ZCRESUM, &offset, saved,
                                  sizeof saved);
            if (!f) {
                zsend_hex_header(s, ZSKIP, (uint8_t[4]){0});
                break;
            }
            start = offset;
            wlen = 0;
            snprintf(s->name, sizeof s->name, "%s", saved);
            s->prog = (xfer_progress_t){s->name, offset, size, s->prog.errors, s->prog.files};
            progress(s);
            zsend_pos(s, ZRPOS, offset);
            break;
        }
        case ZDATA: {
            if (!f) {
                zsend_rinit(s);
                break;
            }
            if (hdr_pos(h) != offset + wlen) {
                purge(s, 100);
                zsend_pos(s, ZRPOS, offset + wlen);
                break;
            }
            for (;;) {
                int len;
                int end = zget_data(s, wbuf + wlen, Z_SUBPACK, &len);
                if (end < 0) {
                    if (end == R_ABORT || end == R_CANCEL) {
                        s->io->close(s->io->ctx, f, false);
                        f = NULL;
                        zabort(s, end, NULL, NULL);
                        done = true;
                        break;
                    }
                    if (++errors > RETRIES * 2) {
                        s->io->close(s->io->ctx, f, false);
                        f = NULL;
                        zabort(s, R_TIMEOUT, "Too many errors.", NULL);
                        done = true;
                        break;
                    }
                    ++s->prog.errors;
                    purge(s, 100);
                    zsend_pos(s, ZRPOS, offset + wlen);
                    break;
                }
                wlen += len;
                errors = 0;  // only errors in a row give up
                s->prog.done = offset + wlen;
                progress(s);
                if (wlen >= Z_BUFLEN || end == ZCRCW || end == ZCRCE) {
                    if (wlen && !s->io->write_file(s->io->ctx, f, wbuf, wlen)) {
                        s->io->close(s->io->ctx, f, false);
                        f = NULL;
                        zabort(s, R_TIMEOUT, "Can't write %s (drive full?).", s->name);
                        done = true;
                        break;
                    }
                    offset += wlen;
                    wlen = 0;
                }
                if (end == ZCRCW || end == ZCRCQ)
                    zsend_pos(s, ZACK, offset + wlen);
                if (end == ZCRCW || end == ZCRCE)
                    break;  // a header comes next
            }
            break;
        }
        case ZEOF:
            if (!f || hdr_pos(h) != offset + wlen)
                break;  // stale: ignore it, as the spec says
            if (wlen && !s->io->write_file(s->io->ctx, f, wbuf, wlen)) {
                s->io->close(s->io->ctx, f, false);
                f = NULL;
                zabort(s, R_TIMEOUT, "Can't write %s (drive full?).", s->name);
                done = true;
                break;
            }
            offset += wlen;
            wlen = 0;
            s->io->close(s->io->ctx, f, true);
            f = NULL;
            file_done(s, offset - start);
            progress(s);
            zsend_rinit(s);
            break;
        case ZFIN:
            zsend_hex_header(s, ZFIN, (uint8_t[4]){0});
            // "OO" (over and out) may follow; read it so it doesn't show.
            for (int i = 0; i < 2 && rd(s, 500) >= 0; ++i)
                ;
            s->res->ok = true;
            done = true;
            break;
        case ZCAN:
        case ZABORT:
            zabort(s, R_CANCEL, NULL, NULL);
            done = true;
            break;
        default: break;  // ZNAK, ZCOMMAND, ...: not for a receiver
        }
    }
    if (f)
        s->io->close(s->io->ctx, f, false);
    free(buf);
    free(wbuf);
    if (s->res->ok)
        snprintf(s->res->message, sizeof s->res->message, "Received %d file%s, %lu bytes.",
                 s->res->files, s->res->files == 1 ? "" : "s", (unsigned long)s->res->bytes);
}

/// Waits for the receiver's next header, answering ZCHALLENGE. Its type or
/// a negative R_ value.
/// @param s the transfer
/// @param h set to its 4 bytes
/// @param timeout_ms how long to wait
/// @return its type, or a negative R_ value
static int zwait(xs_t *s, uint8_t h[4], unsigned timeout_ms) {
    for (;;) {
        int type = zget_header(s, h, timeout_ms);
        if (type == ZCHALLENGE) {
            zsend_hex_header(s, ZACK, h);
            continue;
        }
        return type;
    }
}

/// Sends one file's data from `pos`. 1 done (receiver took ZEOF), 0 skipped,
/// else a negative R_ value.
/// @param s the transfer
/// @param f the file
/// @param size its size
/// @param pos where to start
/// @param rxbuflen the receiver's buffer (0: no limit)
/// @return 1, 0 or a negative R_ value
static int z_send_data(xs_t *s, void *f, uint32_t size, uint32_t pos, unsigned rxbuflen) {
    uint8_t buf[Z_SUBPACK];
    int retries = 0;            // restarts without getting further
    uint32_t furthest = pos;    // the furthest restart point so far
    for (;;) {
        // (Re)start a data frame at pos.
        if (!s->io->seek(s->io->ctx, f, pos))
            return R_BAD;
        uint8_t h[4];
        pos_hdr(h, pos);
        zsend_bin_header(s, ZDATA, h);
        uint32_t since_ack = 0;
        int restart = -1;  // a ZRPOS position
        for (;;) {
            int n = s->io->read(s->io->ctx, f, buf, Z_SUBPACK);
            if (n < 0)
                return R_BAD;
            bool last = pos + n >= size || n == 0;
            uint8_t end = last ? ZCRCE : ZCRCG;
            if (!last && rxbuflen && since_ack + n >= rxbuflen)
                end = ZCRCW;
            zsend_data(s, buf, n, end);
            pos += n;
            since_ack += n;
            s->prog.done = pos;
            progress(s);
            if (end == ZCRCW) {
                int t = zwait(s, h, 10000);
                if (t == ZACK) {
                    since_ack = 0;
                    break;  // new frame from pos
                }
                if (t == ZRPOS) {
                    restart = hdr_pos(h);
                    break;
                }
                if (t == ZSKIP)
                    return 0;
                if (t < 0 && t != R_BAD && t != R_TIMEOUT)
                    return t;
                ++s->prog.errors;
                restart = pos - since_ack;  // no answer: resend from the last ACK
                break;
            }
            if (last)
                break;
            // Anything from the receiver mid-stream is a ZRPOS (or a cancel).
            int c = rd(s, 0);
            if (c == R_ABORT)
                return R_ABORT;
            if (c == ZPAD || c == CAN) {
                int t = zwait(s, h, 1000);
                if (t == ZRPOS) {
                    restart = hdr_pos(h);
                    break;
                }
                if (t == R_CANCEL || t == R_ABORT)
                    return t;
                if (t == ZSKIP)
                    return 0;
            }
        }
        if (restart >= 0) {
            if ((uint32_t)restart > furthest) {
                furthest = restart;
                retries = 0;
            }
            if (++retries > RETRIES)
                return R_TIMEOUT;
            ++s->prog.errors;
            pos = restart;
            continue;
        }
        if (pos < size)
            continue;  // after a ZCRCW's ZACK
        // End of file: ZEOF until the receiver's ZRINIT.
        for (int tries = 0;; ++tries) {
            if (tries == RETRIES)
                return R_TIMEOUT;
            pos_hdr(h, pos);
            zsend_hex_header(s, ZEOF, h);
            int t = zwait(s, h, 10000);
            if (t == ZRINIT)
                return 1;
            if (t == ZRPOS) {
                pos = hdr_pos(h);
                ++s->prog.errors;
                if (pos > furthest) {
                    furthest = pos;
                    retries = 0;
                }
                break;
            }
            if (t == ZSKIP)
                return 0;
            if (t == R_CANCEL || t == R_ABORT)
                return t;
        }
        if (++retries > RETRIES)
            return R_TIMEOUT;
    }
}

/// Sends by ZMODEM.
/// @param s the transfer
/// @param paths the files
/// @param count their count
static void z_send(xs_t *s, const char *const *paths, int count) {
    uint8_t h[4] = {0};
    wr(s, "rz\r", 3);  // starts rz on a Unix shell at the far end
    unsigned rxbuflen = 0;
    int t;
    for (int tries = 0;; ++tries) {
        if (tries == RETRIES) {
            zabort(s, R_TIMEOUT, NULL, NULL);
            return;
        }
        memset(h, 0, 4);
        zsend_hex_header(s, ZRQINIT, h);
        t = zwait(s, h, tries < 2 ? 5000 : 10000);
        if (t == ZRINIT)
            break;
        if (t == R_CANCEL || t == R_ABORT) {
            zabort(s, t, NULL, NULL);
            return;
        }
    }
    rxbuflen = h[0] | h[1] << 8;
    // rz sends ZRINIT when it starts and again for our ZRQINIT: drop the
    // second, or it looks like an answer to the first ZFILE.
    if (purge(s, 300) == R_ABORT) {
        zabort(s, R_ABORT, NULL, NULL);
        return;
    }
    uint32_t bytes_left = 0;
    int skipped = 0;  // files the receiver didn't want (ZSKIP), e.g. it has them
    for (int i = 0; i < count; ++i) {
        uint8_t info[160];
        uint32_t size;
        void *f = s->io->open_read(s->io->ctx, paths[i], &size, s->name, sizeof s->name);
        if (!f) {
            zabort(s, R_TIMEOUT, "Can't open %s.", paths[i]);
            return;
        }
        s->prog = (xfer_progress_t){s->name, 0, size, s->prog.errors, s->prog.files};
        progress(s);
        int n = snprintf((char *)info, 64, "%s", s->name) + 1;
        n += snprintf((char *)info + n, sizeof info - n, "%lu 0 0 0 %d %lu", (unsigned long)size,
                      count - i, (unsigned long)(size + bytes_left)) + 1;
        int r = -1;
        for (int tries = 0; tries < RETRIES && r < 0; ++tries) {
            uint8_t fh[4] = {0, 0, 0, ZCBIN};
            zsend_bin_header(s, ZFILE, fh);
            zsend_data(s, info, n, ZCRCW);
            t = zwait(s, h, 10000);
            if (t == ZRINIT) {
                // Maybe stale (sent before our ZFILE got there): use the
                // next header if one follows straight away.
                int t2 = zwait(s, h, 500);
                if (t2 >= 0 || t2 == R_CANCEL || t2 == R_ABORT)
                    t = t2;
            }
            if (t == ZRPOS)
                r = z_send_data(s, f, size, hdr_pos(h), rxbuflen);
            else if (t == ZSKIP)
                r = 0;
            else if (t == R_CANCEL || t == R_ABORT)
                r = t;
            // ZRINIT, ZNAK, a timeout: send ZFILE again
        }
        s->io->close(s->io->ctx, f, r == 1);
        if (r < 0) {
            zabort(s, r, r == R_TIMEOUT || r == R_BAD ? "Too many errors." : NULL, NULL);
            return;
        }
        if (r == 1)
            file_done(s, size);
        else
            ++skipped;
        progress(s);
    }
    for (int tries = 0; tries < RETRIES; ++tries) {
        memset(h, 0, 4);
        zsend_hex_header(s, ZFIN, h);
        t = zwait(s, h, 5000);
        if (t == ZFIN || t == R_CANCEL || t == R_ABORT)
            break;
    }
    wr(s, "OO", 2);
    s->res->ok = skipped == 0;
    int n = snprintf(s->res->message, sizeof s->res->message, "Sent %d file%s, %lu bytes.",
                     s->res->files, s->res->files == 1 ? "" : "s", (unsigned long)s->res->bytes);
    if (skipped)
        snprintf(s->res->message + n, sizeof s->res->message - n,
                 " %d skipped by the receiver (it may have them).", skipped);
}

// ---- Entry points -------------------------------------------------------------------

void xfer_send(const xfer_io_t *io, xfer_proto_t proto, const char *const *paths, int count,
               xfer_result_t *result) {
    xs_t *s = calloc(1, sizeof *s);
    *result = (xfer_result_t){0};
    if (!s) {
        snprintf(result->message, sizeof result->message, "Out of memory.");
        return;
    }
    s->io = io;
    s->res = result;
    s->prog.name = s->name;
    if (proto == XFER_ZMODEM)
        z_send(s, paths, count);
    else
        xy_send(s, proto, paths, count);
    free(s);
}

void xfer_receive(const xfer_io_t *io, xfer_proto_t proto, const char *xmodem_name,
                  xfer_result_t *result) {
    xs_t *s = calloc(1, sizeof *s);
    *result = (xfer_result_t){0};
    if (!s) {
        snprintf(result->message, sizeof result->message, "Out of memory.");
        return;
    }
    s->io = io;
    s->res = result;
    s->prog.name = s->name;
    if (proto == XFER_ZMODEM)
        z_receive(s);
    else
        xy_receive(s, proto == XFER_YMODEM, xmodem_name);
    free(s);
}
