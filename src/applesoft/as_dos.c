/**
 * @file as_dos.c
 * @brief DOS 3.3 style commands, typed at the ] prompt or printed after CHR$(4) by a program, working on files in the drive's current folder.
 *
 * CATALOG, LOAD/SAVE/RUN/CHAIN for programs, BLOAD/BSAVE/BRUN, EXEC, DELETE,
 * RENAME and the text file commands OPEN, CLOSE, READ, WRITE, APPEND,
 * POSITION.
 *
 * Programs are saved as text (NAME.BAS), so they can be read and edited on a
 * PC or in the OS editor; LOAD also takes tokenised Applesoft files. Text
 * files are plain ASCII with newlines.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_overlay.h"
#include "as_internal.h"
#include "as_port.h"

enum {
    DOS_LANGUAGE = 1, DOS_RANGE = 2, DOS_WRITE_PROTECTED = 4, DOS_END_OF_DATA = 5,
    DOS_NOT_FOUND = 6, DOS_IO = 8, DOS_FULL = 9, DOS_LOCKED = 10, DOS_SYNTAX = 11,
    DOS_NO_BUFFERS = 12, DOS_TYPE = 13, DOS_TOO_LARGE = 14, DOS_NOT_DIRECT = 15,
};

const char *dos_error_text(int code) {
    switch (code) {
    case DOS_LANGUAGE: return "LANGUAGE NOT AVAILABLE";
    case DOS_RANGE: case 3: return "RANGE ERROR";
    case DOS_WRITE_PROTECTED: return "WRITE PROTECTED";
    case DOS_END_OF_DATA: return "END OF DATA";
    case DOS_NOT_FOUND: return "FILE NOT FOUND";
    case 7: return "VOLUME MISMATCH";
    case DOS_IO: return "I/O ERROR";
    case DOS_FULL: return "DISK FULL";
    case DOS_LOCKED: return "FILE LOCKED";
    case DOS_SYNTAX: return "SYNTAX ERROR";
    case DOS_NO_BUFFERS: return "NO BUFFERS AVAILABLE";
    case DOS_TYPE: return "FILE TYPE MISMATCH";
    case DOS_TOO_LARGE: return "PROGRAM TOO LARGE";
    case DOS_NOT_DIRECT: return "NOT DIRECT COMMAND";
    default: return "I/O ERROR";
    }
}

#define MAX_FILES 3

typedef struct {
    bool open;
    char name[64];
    asp_file *f;
    unsigned reclen;   ///< random access record length (0: sequential)
} dos_file;

static dos_file files[MAX_FILES] APP_OV_DATA(apple2);
static dos_file *reading, *writing;
static asp_file *exec_file;
static bool mon_c, mon_i, mon_o;

void dos_init(void) {
    memset(files, 0, sizeof files);
    reading = writing = NULL;
    exec_file = NULL;
}

void as_close_dos_io(void) {
    reading = writing = NULL;
}

void dos_close_all(void) {
    for (int i = 0; i < MAX_FILES; ++i)
        if (files[i].open) {
            asp_close(files[i].f);
            files[i].open = false;
        }
    reading = writing = NULL;
    if (exec_file) {
        asp_close(exec_file);
        exec_file = NULL;
    }
}

bool dos_reading(void) {
    return reading != NULL;
}

bool dos_output(uint8_t c) {
    if (!writing)
        return false;
    uint8_t b = c & 0x7f;
    if (b == '\r')
        b = '\n';
    if (asp_write(writing->f, &b, 1) != 1)
        as_dos_error(DOS_FULL);
    if (mon_o)
        return false;
    return true;
}

int dos_input_char(void) {
    if (reading) {
        uint8_t b;
        int n = asp_read(reading->f, &b, 1);
        if (n != 1) {
            reading = NULL;
            as_dos_error(DOS_END_OF_DATA);
        }
        if (b == '\r') {
            // \r\n is one end of line
            uint32_t at = asp_tell(reading->f);
            uint8_t nx;
            if (asp_read(reading->f, &nx, 1) == 1 && nx != '\n')
                asp_seek(reading->f, at);
            b = '\n';
        }
        if (mon_i)
            as_cout((uint8_t)((b == '\n' ? '\r' : b) | 0x80));
        return b == '\n' ? '\r' : b;
    }
    if (exec_file) {
        uint8_t b;
        if (asp_read(exec_file, &b, 1) != 1) {
            asp_close(exec_file);
            exec_file = NULL;
            return -1;
        }
        if (b == '\n')
            b = '\r';
        else if (b == '\r') {
            uint32_t at = asp_tell(exec_file);
            uint8_t nx;
            if (asp_read(exec_file, &nx, 1) == 1 && nx != '\n')
                asp_seek(exec_file, at);
        }
        return b;
    }
    return -1;
}

// ---- Command parsing ----------------------------------------------------------

typedef struct {
    char name[64];
    char name2[64];  ///< RENAME's second name
    bool has[26];    ///< options A B D L R S V...
    long val[26];
} dos_args;

static const char *skip_spaces(const char *p) {
    while (*p == ' ')
        p++;
    return p;
}

/// name[,name2],options. Option values are decimal, or hex after '$'.
static void parse_args(const char *p, dos_args *a, bool two_names) {
    memset(a, 0, sizeof *a);
    p = skip_spaces(p);
    int n = 0;
    while (*p && *p != ',' && n < 63)
        a->name[n++] = *p++;
    while (n && a->name[n - 1] == ' ')
        n--;
    a->name[n] = 0;
    if (two_names && *p == ',') {
        p = skip_spaces(p + 1);
        n = 0;
        while (*p && *p != ',' && n < 63)
            a->name2[n++] = *p++;
        while (n && a->name2[n - 1] == ' ')
            n--;
        a->name2[n] = 0;
    }
    while (*p == ',') {
        p = skip_spaces(p + 1);
        int o = toupper((unsigned char)*p) - 'A';
        if (o < 0 || o >= 26)
            as_dos_error(DOS_SYNTAX);
        p = skip_spaces(p + 1);
        long v = 0;
        if (*p == '$') {
            p++;
            if (!isxdigit((unsigned char)*p))
                as_dos_error(DOS_SYNTAX);
            while (isxdigit((unsigned char)*p)) {
                int d = isdigit((unsigned char)*p) ? *p - '0' : toupper((unsigned char)*p) - 'A' + 10;
                v = v * 16 + d;
                p++;
            }
        } else {
            if (!isdigit((unsigned char)*p))
                as_dos_error(DOS_SYNTAX);
            while (isdigit((unsigned char)*p))
                v = v * 10 + (*p++ - '0');
        }
        if (v > 65535)
            as_dos_error(DOS_RANGE);
        a->has[o] = true;
        a->val[o] = v;
        p = skip_spaces(p);
    }
    if (*p)
        as_dos_error(DOS_SYNTAX);
}

static bool has_extension(const char *name) {
    const char *dot = strrchr(name, '.');
    return dot && !strchr(dot, '/');
}

/// Finds name, or name with ext added. Returns the name to use in out.
static bool find_file(const char *name, const char *ext, char *out, size_t size) {
    if (!*name)
        as_dos_error(DOS_SYNTAX);
    if (asp_exists(name)) {
        snprintf(out, size, "%s", name);
        return true;
    }
    if (ext && !has_extension(name)) {
        snprintf(out, size, "%s%s", name, ext);
        if (asp_exists(out))
            return true;
    }
    snprintf(out, size, "%s%s", name, ext && !has_extension(name) ? ext : "");
    return false;
}

// ---- Programs ------------------------------------------------------------------

static void load_text_program(asp_file *f) {
    char line[512];
    int n = 0;
    uint8_t buf[256];
    int got;
    as_new_program();
    while ((got = asp_read(f, buf, sizeof buf)) > 0) {
        for (int i = 0; i < got; ++i) {
            char c = (char)(buf[i] & 0x7f);
            if (c == '\n' || c == '\r') {
                line[n] = 0;
                if (n)
                    as_enter_line(line);
                n = 0;
            } else if (n < (int)sizeof line - 1) {
                line[n++] = c;
            }
        }
    }
    line[n] = 0;
    if (n)
        as_enter_line(line);
}

/// A tokenised Applesoft file: as DOS 3.3 keeps them, a 2-byte length then
/// the program as it sits in memory; or the bare program.
static bool load_tokenised(const uint8_t *data, uint32_t size) {
    const uint8_t *p = data;
    uint32_t len = size;
    if (size >= 2 && (uint32_t)(data[0] | data[1] << 8) + 2 <= size &&
        (uint32_t)(data[0] | data[1] << 8) + 2 >= size - 256) {
        len = data[0] | data[1] << 8;
        p = data + 2;
    }
    uint16_t start = w16(Z_TXTTAB);
    if (start + len + 2 >= w16(Z_MEMSIZ))
        as_dos_error(DOS_TOO_LARGE);
    memcpy(&M[start], p, len);
    M[start + len] = 0;
    M[start + len + 1] = 0;
    // Find the end by following line numbers (the links may point anywhere).
    uint16_t q = start;
    while (q < start + len && M[q + 1]) {
        uint16_t r = q + 4;
        while (r < start + len && M[r])
            r++;
        q = r + 1;
    }
    M[q] = M[q + 1] = 0;
    sw16(Z_VARTAB, q + 2);
    as_fix_links();
    as_clear();
    return true;
}

static bool looks_tokenised(const uint8_t *d, uint32_t size) {
    // Text programs start with a digit (or blank lines); tokenised ones with
    // a link and a line number, and contain bytes >= $80.
    uint32_t i = 0;
    while (i < size && (d[i] == '\n' || d[i] == '\r' || d[i] == ' '))
        i++;
    if (i < size && isdigit(d[i]))
        return false;
    for (uint32_t k = 0; k < size && k < 4096; ++k)
        if (d[k] >= 0x80)
            return true;
    return false;
}

void dos_load_program(const char *name) {
    char path[80];
    if (!find_file(name, ".BAS", path, sizeof path))
        as_dos_error(DOS_NOT_FOUND);
    asp_file *f = asp_open(path, ASP_READ);
    if (!f)
        as_dos_error(DOS_NOT_FOUND);
    uint32_t size = asp_size(f);
    uint8_t head[512];
    int got = asp_read(f, head, sizeof head);
    if (got < 0) {
        asp_close(f);
        as_dos_error(DOS_IO);
    }
    if (looks_tokenised(head, (uint32_t)got)) {
        if (size > 0xa000) {
            asp_close(f);
            as_dos_error(DOS_TOO_LARGE);
        }
        uint8_t *whole = asp_alloc(size + 1);
        if (!whole) {
            asp_close(f);
            as_dos_error(DOS_TOO_LARGE);
        }
        asp_seek(f, 0);
        uint32_t have = 0;
        while (have < size) {
            int r = asp_read(f, whole + have, (int)(size - have));
            if (r <= 0)
                break;
            have += (uint32_t)r;
        }
        asp_close(f);
        load_tokenised(whole, have);
        asp_free(whole);
        return;
    }
    asp_seek(f, 0);
    load_text_program(f);
    asp_close(f);
}

void dos_save_program(const char *name) {
    char path[80];
    find_file(name, ".BAS", path, sizeof path);
    asp_file *f = asp_open(path, ASP_WRITE);
    if (!f)
        as_dos_error(DOS_IO);
    uint16_t p = w16(Z_TXTTAB);
    char line[600];
    while (M[(uint16_t)(p + 1)]) {
        int n = as_detok_line(p, line, sizeof line - 1);
        line[n++] = '\n';
        if (asp_write(f, line, n) != n) {
            asp_close(f);
            as_dos_error(DOS_FULL);
        }
        p = w16(p);
    }
    if (!asp_close(f))
        as_dos_error(DOS_IO);
}

// ---- Binary files ------------------------------------------------------------------
// A DOS 3.3 binary file starts with its load address and length; files
// without that header need ,A (and are read to their end).

static void bload(dos_args *a, bool run) {
    char path[80];
    if (!find_file(a->name, ".BIN", path, sizeof path))
        as_dos_error(DOS_NOT_FOUND);
    asp_file *f = asp_open(path, ASP_READ);
    if (!f)
        as_dos_error(DOS_NOT_FOUND);
    uint32_t size = asp_size(f);
    uint8_t h[4];
    uint32_t addr = 0, len = size;
    bool header = false;
    if (size >= 4 && asp_read(f, h, 4) == 4) {
        uint32_t ha = h[0] | h[1] << 8, hl = h[2] | h[3] << 8;
        if (hl + 4 == size) {
            header = true;
            addr = ha;
            len = hl;
        }
    }
    if (!header) {
        asp_seek(f, 0);
        if (!a->has['A' - 'A']) {
            asp_close(f);
            as_dos_error(DOS_RANGE);
        }
    }
    if (a->has['A' - 'A'])
        addr = (uint32_t)a->val['A' - 'A'];
    if (a->has['L' - 'A'] && (uint32_t)a->val['L' - 'A'] < len)
        len = (uint32_t)a->val['L' - 'A'];
    if (addr + len > 0x10000)
        len = 0x10000 - addr;
    uint32_t done = 0;
    while (done < len) {
        uint8_t buf[256];
        int want = len - done > sizeof buf ? (int)sizeof buf : (int)(len - done);
        int r = asp_read(f, buf, want);
        if (r <= 0)
            break;
        for (int i = 0; i < r; ++i)
            as_poke((uint16_t)(addr + done + i), buf[i]);
        done += (uint32_t)r;
    }
    asp_close(f);
    // DOS keeps the last BLOAD's address and length at $AA72/$AA60.
    sw16(0xaa72, (uint16_t)addr);
    sw16(0xaa60, (uint16_t)len);
    if (run)
        as_call((uint16_t)addr);
}

static void bsave(dos_args *a) {
    if (!a->has['A' - 'A'] || !a->has['L' - 'A'])
        as_dos_error(DOS_SYNTAX);
    char path[80];
    find_file(a->name, ".BIN", path, sizeof path);
    asp_file *f = asp_open(path, ASP_WRITE);
    if (!f)
        as_dos_error(DOS_IO);
    uint16_t addr = (uint16_t)a->val['A' - 'A'];
    uint16_t len = (uint16_t)a->val['L' - 'A'];
    uint8_t h[4] = {(uint8_t)addr, (uint8_t)(addr >> 8), (uint8_t)len, (uint8_t)(len >> 8)};
    bool ok = asp_write(f, h, 4) == 4;
    for (uint32_t i = 0; ok && i < len; ++i) {
        uint8_t b = M[(uint16_t)(addr + i)];
        ok = asp_write(f, &b, 1) == 1;
    }
    if (!asp_close(f) || !ok)
        as_dos_error(DOS_FULL);
}

// ---- CATALOG -----------------------------------------------------------------------

static int cat_lines;

static void cat_entry(void *ctx, const char *name, uint32_t size, bool is_dir) {
    (void)ctx;
    char type = 'T';
    const char *dot = strrchr(name, '.');
    if (is_dir)
        type = 'D';
    else if (dot && (!strcasecmp(dot, ".BAS") || !strcasecmp(dot, ".APP")))
        type = 'A';
    else if (dot && (!strcasecmp(dot, ".BIN") || !strcasecmp(dot, ".OBJ")))
        type = 'B';
    else if (dot && !strcasecmp(dot, ".INT"))
        type = 'I';
    unsigned sectors = is_dir ? 1 : (size + 255) / 256 + 1;
    if (sectors > 999)
        sectors = 999;
    char line[96];
    snprintf(line, sizeof line, " %c %03u %s", type, sectors, name);
    for (char *p = line; *p; ++p)
        as_outdo((uint8_t)toupper((unsigned char)*p));
    as_crdo();
    // DOS pauses each screenful.
    if (++cat_lines >= 21) {
        cat_lines = 0;
        as_rdkey();
    }
}

static void catalog(void) {
    as_crdo();
    as_print("DISK VOLUME 254");
    as_crdo();
    as_crdo();
    cat_lines = 3;
    if (!asp_list(cat_entry, NULL))
        as_dos_error(DOS_IO);
}

// ---- Text files ----------------------------------------------------------------------

static dos_file *find_open(const char *name) {
    for (int i = 0; i < MAX_FILES; ++i)
        if (files[i].open && !strcasecmp(files[i].name, name))
            return &files[i];
    return NULL;
}

static void close_file(dos_file *d) {
    if (!d->open)
        return;
    if (reading == d)
        reading = NULL;
    if (writing == d)
        writing = NULL;
    asp_close(d->f);
    d->open = false;
}

static void open_file(dos_args *a) {
    dos_file *d = find_open(a->name);
    if (d)
        close_file(d);
    for (int i = 0; i < MAX_FILES && !d; ++i)
        if (!files[i].open)
            d = &files[i];
    if (!d)
        as_dos_error(DOS_NO_BUFFERS);
    asp_file *f = asp_open(a->name, ASP_UPDATE);
    if (!f)
        as_dos_error(DOS_IO);
    d->open = true;
    d->f = f;
    d->reclen = a->has['L' - 'A'] ? (unsigned)a->val['L' - 'A'] : 0;
    snprintf(d->name, sizeof d->name, "%s", a->name);
}

static dos_file *need_open(const char *name) {
    dos_file *d = find_open(name);
    if (!d)
        as_dos_error(DOS_NOT_FOUND);
    return d;
}

static void position_for(dos_file *d, dos_args *a) {
    if (a->has['R' - 'A']) {
        if (!d->reclen && !a->has['B' - 'A']) {
            // Sequential: R counts lines from here.
            long lines = a->val['R' - 'A'];
            while (lines > 0) {
                uint8_t b;
                if (asp_read(d->f, &b, 1) != 1)
                    as_dos_error(DOS_END_OF_DATA);
                if (b == '\n')
                    lines--;
            }
        } else {
            asp_seek(d->f, (uint32_t)(a->val['R' - 'A'] * (long)d->reclen));
        }
    }
    if (a->has['B' - 'A'])
        asp_seek(d->f, asp_tell(d->f) + (uint32_t)a->val['B' - 'A']);
}

// ---- The command table ----------------------------------------------------------

enum {
    C_CATALOG, C_CAT, C_LOAD, C_SAVE, C_RUN, C_CHAIN, C_DELETE, C_RENAME, C_LOCK, C_UNLOCK,
    C_VERIFY, C_BLOAD, C_BSAVE, C_BRUN, C_EXEC, C_OPEN, C_CLOSE, C_READ, C_WRITE, C_APPEND,
    C_POSITION, C_MON, C_NOMON, C_MAXFILES, C_FP, C_INT, C_INIT, C_PR, C_IN, C_BYE,
    C_PREFIX,
};

static const struct {
    const char *word;
    int id;
} commands[] = {
    {"CATALOG", C_CATALOG}, {"CAT", C_CAT}, {"LOAD", C_LOAD}, {"SAVE", C_SAVE},
    {"RUN", C_RUN}, {"CHAIN", C_CHAIN}, {"DELETE", C_DELETE}, {"RENAME", C_RENAME},
    {"LOCK", C_LOCK}, {"UNLOCK", C_UNLOCK}, {"VERIFY", C_VERIFY}, {"BLOAD", C_BLOAD},
    {"BSAVE", C_BSAVE}, {"BRUN", C_BRUN}, {"EXEC", C_EXEC}, {"OPEN", C_OPEN},
    {"CLOSE", C_CLOSE}, {"READ", C_READ}, {"WRITE", C_WRITE}, {"APPEND", C_APPEND},
    {"POSITION", C_POSITION}, {"MON", C_MON}, {"NOMON", C_NOMON}, {"MAXFILES", C_MAXFILES},
    {"FP", C_FP}, {"INT", C_INT}, {"INIT", C_INIT}, {"PR#", C_PR}, {"IN#", C_IN},
    {"BYE", C_BYE}, {"PREFIX", C_PREFIX},
};

/// Matches a command word at the start of line (spaces ignored, any case).
static int match_command(const char *line, const char **rest) {
    for (size_t i = 0; i < sizeof commands / sizeof commands[0]; ++i) {
        const char *w = commands[i].word, *p = line;
        while (*w) {
            while (*p == ' ')
                p++;
            if (toupper((unsigned char)*p) != *w)
                break;
            p++;
            w++;
        }
        if (!*w) {
            *rest = p;
            return commands[i].id;
        }
    }
    return -1;
}

static void do_monitor_flags(const char *p, bool on) {
    for (; *p; ++p) {
        char c = (char)toupper((unsigned char)*p);
        if (c == 'C')
            mon_c = on;
        else if (c == 'I')
            mon_i = on;
        else if (c == 'O')
            mon_o = on;
    }
}

bool dos_command(const char *line, bool from_program) {
    const char *rest;
    int id = match_command(line, &rest);
    if (id < 0) {
        if (from_program) {
            // An empty command (just CHR$(4)) ends READ/WRITE; anything else
            // is a DOS syntax error.
            if (*skip_spaces(line))
                as_dos_error(DOS_SYNTAX);
            reading = writing = NULL;
            return true;
        }
        return false;
    }
    const char *args = skip_spaces(rest);
    // RUN without a name, or with a line number, is BASIC's.
    if ((id == C_RUN) && (!*args || isdigit((unsigned char)*args))) {
        if (from_program)
            as_dos_error(DOS_SYNTAX);
        return false;
    }
    // Any DOS command ends a READ or WRITE.
    reading = writing = NULL;
    if (mon_c && from_program) {
        for (const char *p = line; *p; ++p)
            as_cout((uint8_t)(*p | 0x80));
        as_cout(0x8d);
    }
    dos_args a;
    switch (id) {
    case C_CATALOG: case C_CAT:
        catalog();
        break;
    case C_LOAD:
        parse_args(args, &a, false);
        dos_close_all();
        dos_load_program(a.name);
        if (from_program)
            as_quit_to_prompt();
        break;
    case C_SAVE:
        parse_args(args, &a, false);
        dos_save_program(a.name);
        break;
    case C_RUN:
        parse_args(args, &a, false);
        dos_close_all();
        dos_load_program(a.name);
        as_start_run();
        break;
    case C_CHAIN:
        parse_args(args, &a, false);
        as_chain(a.name);
        break;
    case C_DELETE: {
        parse_args(args, &a, false);
        char path[80];
        if (!find_file(a.name, NULL, path, sizeof path) &&
            !find_file(a.name, ".BAS", path, sizeof path))
            as_dos_error(DOS_NOT_FOUND);
        dos_file *d = find_open(a.name);
        if (d)
            close_file(d);
        if (!asp_remove(path))
            as_dos_error(DOS_IO);
        break;
    }
    case C_RENAME: {
        parse_args(args, &a, true);
        if (!*a.name2)
            as_dos_error(DOS_SYNTAX);
        char path[80];
        if (!find_file(a.name, NULL, path, sizeof path))
            as_dos_error(DOS_NOT_FOUND);
        if (!asp_rename(path, a.name2))
            as_dos_error(DOS_IO);
        break;
    }
    case C_LOCK: case C_UNLOCK: case C_VERIFY: {
        parse_args(args, &a, false);
        char path[80];
        if (!find_file(a.name, ".BAS", path, sizeof path))
            as_dos_error(DOS_NOT_FOUND);
        break;
    }
    case C_BLOAD: parse_args(args, &a, false); bload(&a, false); break;
    case C_BRUN: parse_args(args, &a, false); bload(&a, true); break;
    case C_BSAVE: parse_args(args, &a, false); bsave(&a); break;
    case C_EXEC: {
        parse_args(args, &a, false);
        char path[80];
        if (!find_file(a.name, ".TXT", path, sizeof path))
            as_dos_error(DOS_NOT_FOUND);
        if (exec_file)
            asp_close(exec_file);
        exec_file = asp_open(path, ASP_READ);
        if (!exec_file)
            as_dos_error(DOS_NOT_FOUND);
        break;
    }
    case C_OPEN:
        if (!from_program)
            as_dos_error(DOS_NOT_DIRECT);
        parse_args(args, &a, false);
        open_file(&a);
        break;
    case C_CLOSE:
        if (*args) {
            parse_args(args, &a, false);
            dos_file *d = find_open(a.name);
            if (d)
                close_file(d);
        } else {
            for (int i = 0; i < MAX_FILES; ++i)
                close_file(&files[i]);
        }
        break;
    case C_READ: case C_WRITE: case C_APPEND: case C_POSITION: {
        if (!from_program)
            as_dos_error(DOS_NOT_DIRECT);
        parse_args(args, &a, false);
        dos_file *d;
        if (id == C_APPEND) {
            d = find_open(a.name);
            if (!d) {
                open_file(&a);
                d = find_open(a.name);
            }
            asp_seek(d->f, asp_size(d->f));
            writing = d;
            break;
        }
        d = need_open(a.name);
        position_for(d, &a);
        if (id == C_READ)
            reading = d;
        else if (id == C_WRITE)
            writing = d;
        break;
    }
    case C_MON: do_monitor_flags(args, true); break;
    case C_NOMON: do_monitor_flags(args, false); break;
    case C_MAXFILES: break;
    case C_FP: break;
    case C_INT: as_dos_error(DOS_LANGUAGE);
    case C_INIT: as_dos_error(DOS_WRITE_PROTECTED);
    case C_PR: {
        int n = atoi(args);
        if (n < 0 || n > 7)
            as_dos_error(DOS_RANGE);
        as_pr(n);
        break;
    }
    case C_IN: {
        int n = atoi(args);
        if (n < 0 || n > 7)
            as_dos_error(DOS_RANGE);
        as_in(n);
        break;
    }
    case C_BYE:
        as_quit();
    case C_PREFIX:
        break;
    }
    return true;
}
