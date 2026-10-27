/**
 * @file smb_vfs.c
 * @brief Network shares in the file tree.
 *
 * The program is linked with --wrap=f_open,f_read,... (CMakeLists.txt), so
 * every FatFs call made outside ff.c comes here first: paths on a mounted
 * share (/NET/...) go to the SMB client, everything else to FatFs as before.
 * The shell, editor, BASIC, term, scp and wget all work on shares without
 * knowing.
 *
 * Also the mount and umount commands.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ff.h"
#include "mbedtls/platform_util.h"
#include "net.h"
#include "pico/stdlib.h"
#include "psram_heap.h"
#include "smb.h"
#include "smb_vfs.h"
#include "shell.h"

FRESULT __real_f_open(FIL *fp, const TCHAR *path, BYTE mode);
FRESULT __real_f_close(FIL *fp);
FRESULT __real_f_read(FIL *fp, void *buff, UINT btr, UINT *br);
FRESULT __real_f_write(FIL *fp, const void *buff, UINT btw, UINT *bw);
FRESULT __real_f_lseek(FIL *fp, FSIZE_t ofs);
FRESULT __real_f_sync(FIL *fp);
FRESULT __real_f_opendir(DIR *dp, const TCHAR *path);
FRESULT __real_f_closedir(DIR *dp);
FRESULT __real_f_readdir(DIR *dp, FILINFO *fno);
FRESULT __real_f_stat(const TCHAR *path, FILINFO *fno);
FRESULT __real_f_unlink(const TCHAR *path);
FRESULT __real_f_rename(const TCHAR *path_old, const TCHAR *path_new);
FRESULT __real_f_mkdir(const TCHAR *path);
FRESULT __real_f_chdir(const TCHAR *path);
FRESULT __real_f_getcwd(TCHAR *buff, UINT len);
TCHAR *__real_f_gets(TCHAR *buff, int len, FIL *fp);

#define MAX_MOUNTS 4
#define MAX_FILES  8
#define MAX_DIRS   4
#define PATH_MAX_  300
#define CREDS_FILE "/SMB/CREDS"

typedef struct {
    bool used;
    char point[24];  ///< "/NET"
    char host[64], share[80], domain[32], user[64], password[64];
    uint16_t port;
    smb_t *smb;
    unsigned gen;  ///< bumped on each (re)connection: older handles are stale
} mount_t;

static mount_t *mounts;  ///< MAX_MOUNTS of them, in PSRAM from the first mount on
static int mount_count;
static char vcwd[PATH_MAX_];  ///< the current folder when it is on a share, else ""

static FATFS share_fs;  ///< what an SMB FIL's obj.fs points to

static struct {
    FIL *fp;
    mount_t *m;
    unsigned gen;
    smb_fid_t fid;
    /// f_gets reads ahead into this
    uint8_t *cache;
    FSIZE_t cache_at;
    UINT cache_len;
} files[MAX_FILES];

typedef struct {
    smb_info_t info;
    char name[];  ///< NUL-terminated, then the next entry (4-aligned)
} entry_t;

static struct {
    DIR *dp;
    bool root;     ///< FatFs's root: the mount points are listed after its entries
    int next;      ///< root: the next mount to list
    uint8_t *list;  ///< share folder: its entries (PSRAM)
    size_t len, cap, pos;
} dirs[MAX_DIRS];

// ---- Paths -------------------------------------------------------------------------------

static void normalise(char *path) {
    // Splits on / and \, drops "." and empty parts, and applies "..".
    char out[PATH_MAX_];
    size_t n = 0;
    const char *p = path;
    while (*p) {
        while (*p == '/' || *p == '\\')
            ++p;
        const char *start = p;
        while (*p && *p != '/' && *p != '\\')
            ++p;
        size_t len = p - start;
        if (!len || (len == 1 && start[0] == '.'))
            continue;
        if (len == 2 && start[0] == '.' && start[1] == '.') {
            while (n > 0 && out[n - 1] != '/')
                --n;
            if (n > 0)
                --n;
            continue;
        }
        if (n + 1 + len >= sizeof out)
            break;
        out[n++] = '/';
        memcpy(out + n, start, len);
        n += len;
    }
    if (!n)
        out[n++] = '/';
    out[n] = 0;
    strcpy(path, out);
}

static void make_absolute(const char *path, char *out) {
    if (path[0] == '/' || path[0] == '\\') {
        snprintf(out, PATH_MAX_, "%s", path);
    } else {
        char base[PATH_MAX_];
        if (vcwd[0])
            snprintf(base, sizeof base, "%s", vcwd);
        else if (__real_f_getcwd(base, sizeof base) != FR_OK)
            strcpy(base, "/");
        size_t bl = strlen(base), pl = strlen(path);
        if (bl + 1 + pl >= PATH_MAX_)
            pl = PATH_MAX_ - 2 - bl;  // too long: cut (FatFs will say no)
        memcpy(out, base, bl);
        out[bl] = '/';
        memcpy(out + bl + 1, path, pl);
        out[bl + 1 + pl] = 0;
    }
    normalise(out);
}

static bool same_text(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        char x = a[i], y = b[i];
        if (x >= 'a' && x <= 'z')
            x -= 32;
        if (y >= 'a' && y <= 'z')
            y -= 32;
        if (x != y)
            return false;
    }
    return true;
}

/// Which share a path is on (NULL: FatFs). rel gets the path inside the
/// share ("" for its top); fat_path, what to give FatFs otherwise (the
/// absolute path when the current folder is on a share, as FatFs's own
/// current folder is then somewhere else).
static mount_t *route(const TCHAR *path, char *rel, const TCHAR **fat_path, char *abs_buf) {
    *fat_path = path;
    if (!mount_count && !vcwd[0])
        return NULL;
    if (strchr(path, ':'))
        return NULL;  // a drive number: FatFs's
    make_absolute(path, abs_buf);
    if (vcwd[0])
        *fat_path = abs_buf;
    for (int i = 0; mounts && i < MAX_MOUNTS; ++i) {
        mount_t *m = &mounts[i];
        size_t n = strlen(m->point);
        if (m->used && same_text(abs_buf, m->point, n) && (abs_buf[n] == 0 || abs_buf[n] == '/')) {
            strcpy(rel, abs_buf[n] ? abs_buf + n + 1 : "");  // both PATH_MAX_
            return m;
        }
    }
    return NULL;
}

int smb_mount_points(char (*points)[24], int max) {
    int n = 0;
    for (int i = 0; mounts && i < MAX_MOUNTS && n < max; ++i)
        if (mounts[i].used)
            memcpy(points[n++], mounts[i].point, 24);
    return n;
}

bool smb_path_on_share(const char *path) {
    char rel[PATH_MAX_], abs[PATH_MAX_];
    const TCHAR *fat;
    return route(path, rel, &fat, abs) != NULL;
}

// ---- Connections ------------------------------------------------------------------------

static smb_t *conn(mount_t *m) {
    if (smb_alive(m->smb))
        return m->smb;
    // The connection dropped (or timed out): one try at a new one.
    smb_disconnect(m->smb);
    char err[100];
    m->smb = smb_connect(m->host, m->port, m->share, m->domain, m->user, m->password, err,
                         sizeof err);
    ++m->gen;
    if (!m->smb)
        printf("[%s: %s]\n", m->point, err);
    return m->smb;
}

static FRESULT fr(uint32_t st) {
    switch (st) {
    case SMB_OK: return FR_OK;
    case SMB_NOT_FOUND: return FR_NO_FILE;
    case SMB_PATH_NOT_FOUND:
    case SMB_NOT_A_DIRECTORY: return FR_NO_PATH;
    case SMB_NAME_COLLISION: return FR_EXIST;
    case SMB_ACCESS_DENIED:
    case SMB_FILE_IS_DIRECTORY:
    case SMB_DIR_NOT_EMPTY:
    case SMB_DISK_FULL: return FR_DENIED;
    case SMB_SHARING_VIOLATION: return FR_LOCKED;
    case SMB_NAME_INVALID: return FR_INVALID_NAME;
    case SMB_CONNECTION_LOST:
    case SMB_CANCELLED: return FR_DISK_ERR;
    }
    return FR_INT_ERR;
}

static void fill_info(FILINFO *fno, const char *name, const smb_info_t *in) {
    memset(fno, 0, sizeof *fno);
    fno->fsize = in->size;
    fno->fattrib = in->attrib & (AM_RDO | AM_HID | AM_SYS | AM_DIR | AM_ARC);
    if (in->mtime > 116444736000000000ull) {
        time_t t = (time_t)(in->mtime / 10000000ull - 11644473600ull);
        struct tm tm;
        gmtime_r(&t, &tm);
        if (tm.tm_year >= 80) {
            fno->fdate = (tm.tm_year - 80) << 9 | (tm.tm_mon + 1) << 5 | tm.tm_mday;
            fno->ftime = tm.tm_hour << 11 | tm.tm_min << 5 | tm.tm_sec / 2;
        }
    }
    snprintf(fno->fname, sizeof fno->fname, "%s", name);
}

// ---- Files --------------------------------------------------------------------------------

static int file_slot(FIL *fp) {
    if (fp->obj.fs != &share_fs)
        return -1;
    for (int i = 0; i < MAX_FILES; ++i)
        if (files[i].fp == fp)
            return i;
    return -1;
}

/// The slot's connection if its handle is still good, else NULL.
static smb_t *file_conn(int i) {
    mount_t *m = files[i].m;
    if (!m->used || files[i].gen != m->gen || !smb_alive(m->smb))
        return NULL;
    return m->smb;
}

FRESULT __wrap_f_open(FIL *fp, const TCHAR *path, BYTE mode) {
    char rel[PATH_MAX_], abs[PATH_MAX_];
    const TCHAR *fat;
    mount_t *m = route(path, rel, &fat, abs);
    if (!m)
        return __real_f_open(fp, fat, mode);
    int i = 0;
    while (i < MAX_FILES && files[i].fp)
        ++i;
    if (i == MAX_FILES)
        return FR_TOO_MANY_OPEN_FILES;
    smb_t *s = conn(m);
    if (!s)
        return FR_NOT_READY;
    int smode = (mode & FA_READ ? SMB_READ : 0) | (mode & FA_WRITE ? SMB_WRITE : 0);
    if (!smode)
        smode = SMB_READ;
    if ((mode & FA_OPEN_APPEND) == FA_OPEN_APPEND || (mode & FA_OPEN_ALWAYS))
        smode |= SMB_OPEN_ALWAYS;
    else if (mode & FA_CREATE_ALWAYS)
        smode |= SMB_TRUNCATE;
    else if (mode & FA_CREATE_NEW)
        smode |= SMB_CREATE_NEW;
    smb_info_t info;
    smb_fid_t fid;
    uint32_t st = smb_open(s, rel, smode, &fid, &info);
    if (st)
        return fr(st);
    memset(fp, 0, sizeof *fp);
    fp->obj.fs = &share_fs;
    fp->obj.objsize = info.size;
    fp->flag = mode;
    if ((mode & FA_OPEN_APPEND) == FA_OPEN_APPEND)
        fp->fptr = info.size;
    files[i].fp = fp;
    files[i].m = m;
    files[i].gen = m->gen;
    files[i].fid = fid;
    files[i].cache = NULL;
    files[i].cache_len = 0;
    return FR_OK;
}

FRESULT __wrap_f_close(FIL *fp) {
    int i = file_slot(fp);
    if (i < 0)
        return __real_f_close(fp);
    smb_t *s = file_conn(i);
    uint32_t st = s ? smb_close(s, &files[i].fid) : SMB_CONNECTION_LOST;
    psram_buffer_free(files[i].cache);
    files[i].fp = NULL;
    fp->obj.fs = NULL;
    return fr(st);
}

FRESULT __wrap_f_read(FIL *fp, void *buff, UINT btr, UINT *br) {
    *br = 0;
    int i = file_slot(fp);
    if (i < 0)
        return __real_f_read(fp, buff, btr, br);
    smb_t *s = file_conn(i);
    if (!s)
        return FR_DISK_ERR;
    files[i].cache_len = 0;
    uint32_t got;
    uint32_t st = smb_read(s, &files[i].fid, fp->fptr, buff, btr, &got);
    fp->fptr += got;
    *br = got;
    return fr(st);
}

FRESULT __wrap_f_write(FIL *fp, const void *buff, UINT btw, UINT *bw) {
    *bw = 0;
    int i = file_slot(fp);
    if (i < 0)
        return __real_f_write(fp, buff, btw, bw);
    smb_t *s = file_conn(i);
    if (!s)
        return FR_DISK_ERR;
    if (!(fp->flag & FA_WRITE))
        return FR_DENIED;
    files[i].cache_len = 0;
    uint32_t done;
    uint32_t st = smb_write(s, &files[i].fid, fp->fptr, buff, btw, &done);
    fp->fptr += done;
    if (fp->fptr > fp->obj.objsize)
        fp->obj.objsize = fp->fptr;
    *bw = done;
    return fr(st);
}

FRESULT __wrap_f_lseek(FIL *fp, FSIZE_t ofs) {
    int i = file_slot(fp);
    if (i < 0)
        return __real_f_lseek(fp, ofs);
    if (!(fp->flag & FA_WRITE) && ofs > fp->obj.objsize)
        ofs = fp->obj.objsize;  // as FatFs: reading can't go past the end
    fp->fptr = ofs;
    return FR_OK;
}

FRESULT __wrap_f_sync(FIL *fp) {
    int i = file_slot(fp);
    if (i < 0)
        return __real_f_sync(fp);
    smb_t *s = file_conn(i);
    return s ? fr(smb_flush(s, &files[i].fid)) : FR_DISK_ERR;
}

TCHAR *__wrap_f_gets(TCHAR *buff, int len, FIL *fp) {
    int i = file_slot(fp);
    if (i < 0)
        return __real_f_gets(buff, len, fp);
    // A line at a time from a read-ahead buffer (a network read per
    // character would be very slow).
    smb_t *s = file_conn(i);
    if (!s)
        return NULL;
    if (!files[i].cache && !(files[i].cache = psram_buffer_alloc(4096)))
        return NULL;
    int n = 0;
    while (n < len - 1) {
        if (fp->fptr < files[i].cache_at || fp->fptr >= files[i].cache_at + files[i].cache_len) {
            uint32_t got;
            if (smb_read(s, &files[i].fid, fp->fptr, files[i].cache, 4096, &got) || !got)
                break;
            files[i].cache_at = fp->fptr;
            files[i].cache_len = got;
        }
        char c = files[i].cache[fp->fptr - files[i].cache_at];
        ++fp->fptr;
        buff[n++] = c;
        if (c == '\n')
            break;
    }
    buff[n] = 0;
    return n ? buff : NULL;
}

int __wrap_f_printf(FIL *fp, const TCHAR *fmt, ...) {
    // Both kinds of file: format, then write (FatFs's own f_printf would go
    // round this file's f_write).
    char small[160];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(small, sizeof small, fmt, ap);
    va_end(ap);
    if (n < 0)
        return -1;
    char *text = small;
    if (n >= (int)sizeof small) {
        text = malloc(n + 1);
        if (!text)
            return -1;
        va_start(ap, fmt);
        vsnprintf(text, n + 1, fmt, ap);
        va_end(ap);
    }
    UINT done;
    FRESULT r = __wrap_f_write(fp, text, n, &done);
    if (text != small)
        free(text);
    return r == FR_OK && done == (UINT)n ? n : -1;
}

// ---- Folders ------------------------------------------------------------------------------

static int dir_slot(DIR *dp) {
    for (int i = 0; i < MAX_DIRS; ++i)
        if (dirs[i].dp == dp)
            return i;
    return -1;
}

static void add_entry(void *ctx, const char *name, const smb_info_t *info) {
    int i = (int)(intptr_t)ctx;
    size_t need = (sizeof(entry_t) + strlen(name) + 1 + 3) & ~(size_t)3;
    if (dirs[i].len + need > dirs[i].cap) {
        size_t cap = dirs[i].cap ? dirs[i].cap * 2 : 4096;
        while (cap < dirs[i].len + need)
            cap *= 2;
        uint8_t *bigger = psram_realloc(dirs[i].list, cap);
        if (!bigger)
            return;  // out of room: the listing is cut short
        dirs[i].list = bigger;
        dirs[i].cap = cap;
    }
    entry_t *e = (entry_t *)(dirs[i].list + dirs[i].len);
    e->info = *info;
    strcpy(e->name, name);
    dirs[i].len += need;
}

FRESULT __wrap_f_opendir(DIR *dp, const TCHAR *path) {
    char rel[PATH_MAX_], abs[PATH_MAX_];
    const TCHAR *fat;
    mount_t *m = route(path, rel, &fat, abs);
    int i = 0;
    while (i < MAX_DIRS && dirs[i].dp)
        ++i;
    if (!m) {
        FRESULT r = __real_f_opendir(dp, fat);
        if (r == FR_OK && mount_count && i < MAX_DIRS && !strcmp(abs, "/")) {
            dirs[i] = (typeof(dirs[0])){.dp = dp, .root = true};  // list the mounts too
        }
        return r;
    }
    if (i == MAX_DIRS)
        return FR_TOO_MANY_OPEN_FILES;
    smb_t *s = conn(m);
    if (!s)
        return FR_NOT_READY;
    smb_fid_t fid;
    smb_info_t info;
    uint32_t st = smb_open(s, rel, SMB_READ | SMB_DIRECTORY, &fid, &info);
    if (st)
        return fr(st);
    dirs[i] = (typeof(dirs[0])){.dp = dp};
    // The whole listing is read now (into PSRAM), so the handle can go.
    bool first = true;
    while ((st = smb_list(s, &fid, first, add_entry, (void *)(intptr_t)i)) == SMB_OK)
        first = false;
    smb_close(s, &fid);
    if (st != SMB_NO_MORE_FILES) {
        psram_free(dirs[i].list);
        dirs[i].dp = NULL;
        return fr(st);
    }
    memset(dp, 0, sizeof *dp);
    dp->obj.fs = &share_fs;
    return FR_OK;
}

FRESULT __wrap_f_readdir(DIR *dp, FILINFO *fno) {
    int i = dir_slot(dp);
    if (i < 0)
        return __real_f_readdir(dp, fno);
    if (dirs[i].root) {
        FRESULT r = __real_f_readdir(dp, fno);
        if (!fno) {
            dirs[i].next = 0;
            return r;
        }
        if (r != FR_OK || fno->fname[0])
            return r;
        while (dirs[i].next < MAX_MOUNTS && !mounts[dirs[i].next].used)
            ++dirs[i].next;
        if (dirs[i].next < MAX_MOUNTS) {
            smb_info_t info = {0, SMB_ATTR_DIR, 0};
            fill_info(fno, mounts[dirs[i].next].point + 1, &info);
            ++dirs[i].next;
        }
        return FR_OK;
    }
    if (!fno) {
        dirs[i].pos = 0;
        return FR_OK;
    }
    if (dirs[i].pos >= dirs[i].len) {
        memset(fno, 0, sizeof *fno);
        return FR_OK;
    }
    entry_t *e = (entry_t *)(dirs[i].list + dirs[i].pos);
    fill_info(fno, e->name, &e->info);
    dirs[i].pos += (sizeof(entry_t) + strlen(e->name) + 1 + 3) & ~(size_t)3;
    return FR_OK;
}

FRESULT __wrap_f_closedir(DIR *dp) {
    int i = dir_slot(dp);
    if (i < 0)
        return __real_f_closedir(dp);
    bool root = dirs[i].root;
    psram_free(dirs[i].list);
    dirs[i] = (typeof(dirs[0])){0};
    if (root)
        return __real_f_closedir(dp);
    dp->obj.fs = NULL;
    return FR_OK;
}

// ---- Names ------------------------------------------------------------------------------------

FRESULT __wrap_f_stat(const TCHAR *path, FILINFO *fno) {
    char rel[PATH_MAX_], abs[PATH_MAX_];
    const TCHAR *fat;
    mount_t *m = route(path, rel, &fat, abs);
    if (!m)
        return __real_f_stat(fat, fno);
    FILINFO dummy;
    if (!fno)
        fno = &dummy;
    if (!rel[0]) {  // the mount point itself
        smb_info_t info = {0, SMB_ATTR_DIR, 0};
        fill_info(fno, m->point + 1, &info);
        return FR_OK;
    }
    smb_t *s = conn(m);
    if (!s)
        return FR_NOT_READY;
    smb_info_t info;
    uint32_t st = smb_stat(s, rel, &info);
    if (st)
        return fr(st);
    const char *slash = strrchr(rel, '/');
    fill_info(fno, slash ? slash + 1 : rel, &info);
    return FR_OK;
}

FRESULT __wrap_f_unlink(const TCHAR *path) {
    char rel[PATH_MAX_], abs[PATH_MAX_];
    const TCHAR *fat;
    mount_t *m = route(path, rel, &fat, abs);
    if (!m)
        return __real_f_unlink(fat);
    if (!rel[0])
        return FR_DENIED;
    smb_t *s = conn(m);
    return s ? fr(smb_delete(s, rel)) : FR_NOT_READY;
}

FRESULT __wrap_f_mkdir(const TCHAR *path) {
    char rel[PATH_MAX_], abs[PATH_MAX_];
    const TCHAR *fat;
    mount_t *m = route(path, rel, &fat, abs);
    if (!m)
        return __real_f_mkdir(fat);
    if (!rel[0])
        return FR_EXIST;
    smb_t *s = conn(m);
    return s ? fr(smb_mkdir(s, rel)) : FR_NOT_READY;
}

FRESULT __wrap_f_rename(const TCHAR *path_old, const TCHAR *path_new) {
    char rel1[PATH_MAX_], rel2[PATH_MAX_], abs1[PATH_MAX_], abs2[PATH_MAX_];
    const TCHAR *fat1, *fat2;
    mount_t *m1 = route(path_old, rel1, &fat1, abs1);
    mount_t *m2 = route(path_new, rel2, &fat2, abs2);
    if (!m1 && !m2)
        return __real_f_rename(fat1, fat2);
    if (m1 != m2 || !rel1[0] || !rel2[0])
        return FR_INVALID_DRIVE;  // across drives: copy, then delete
    smb_t *s = conn(m1);
    return s ? fr(smb_rename(s, rel1, rel2)) : FR_NOT_READY;
}

FRESULT __wrap_f_chdir(const TCHAR *path) {
    char rel[PATH_MAX_], abs[PATH_MAX_];
    const TCHAR *fat;
    mount_t *m = route(path, rel, &fat, abs);
    if (!m) {
        FRESULT r = __real_f_chdir(fat);
        if (r == FR_OK)
            vcwd[0] = 0;
        return r;
    }
    if (rel[0]) {
        smb_t *s = conn(m);
        if (!s)
            return FR_NOT_READY;
        smb_info_t info;
        uint32_t st = smb_stat(s, rel, &info);
        if (st)
            return st == SMB_NOT_FOUND ? FR_NO_PATH : fr(st);
        if (!(info.attrib & SMB_ATTR_DIR))
            return FR_NO_PATH;
    }
    snprintf(vcwd, sizeof vcwd, "%s", abs);
    return FR_OK;
}

FRESULT __wrap_f_getcwd(TCHAR *buff, UINT len) {
    if (!vcwd[0])
        return __real_f_getcwd(buff, len);
    if (strlen(vcwd) >= len)
        return FR_NOT_ENOUGH_CORE;
    strcpy(buff, vcwd);
    return FR_OK;
}

// ---- mount and umount ----------------------------------------------------------------------

static bool read_line(char *buf, size_t max, bool secret) {
    size_t n = 0;
    fflush(stdout);
    for (;;) {
        int c = getchar_timeout_us(100000);
        if (c < 0)
            continue;
        if (c == '\r' || c == '\n') {
            buf[n] = 0;
            printf("\n");
            return true;
        }
        if (c == 3 || c == 0x1b) {
            mbedtls_platform_zeroize(buf, max);
            printf("\n");
            return false;
        }
        if ((c == '\b' || c == 0x7f) && n) {
            --n;
            if (!secret)
                printf("\b \b");
        } else if (c >= 0x20 && c != 0x7f && n < max - 1) {
            buf[n++] = c;
            if (!secret)
                putchar(c);
        }
        fflush(stdout);
    }
}

/// /SMB/CREDS: one line per share, "//server/share domain\user password"
/// (plain text, as WIFI.CFG). Fills user/domain/password if the share is
/// there.
static bool find_creds(const char *key, char *domain, char *user, char *password) {
    FIL f;
    if (__wrap_f_open(&f, CREDS_FILE, FA_READ) != FR_OK)
        return false;
    char line[256];
    bool found = false;
    while (!found && __wrap_f_gets(line, sizeof line, &f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *sp = strchr(line, ' ');
        if (!sp)
            continue;
        *sp = 0;
        if (strcasecmp(line, key))
            continue;
        char *who = sp + 1, *pw = strchr(who, ' ');
        if (pw)
            *pw++ = 0;
        char *bs = strchr(who, '\\');
        if (bs) {
            *bs = 0;
            snprintf(domain, 32, "%s", who);
            who = bs + 1;
        }
        snprintf(user, 64, "%s", who);
        snprintf(password, 64, "%s", pw ? pw : "");
        found = true;
    }
    __wrap_f_close(&f);
    mbedtls_platform_zeroize(line, sizeof line);
    return found;
}

static void save_creds(const char *key, const char *domain, const char *user, const char *password) {
    __wrap_f_mkdir("/SMB");
    FIL f;
    if (__wrap_f_open(&f, CREDS_FILE, FA_WRITE | FA_OPEN_APPEND) != FR_OK) {
        printf("Can't write %s\n", CREDS_FILE);
        return;
    }
    __wrap_f_printf(&f, "%s %s%s%s %s\n", key, domain, domain[0] ? "\\" : "", user, password);
    __wrap_f_close(&f);
    printf("Saved in %s (in plain text: keep the drive safe).\n", CREDS_FILE);
}

static void list_mounts(void) {
    if (!mount_count || !mounts) {
        printf("No shares mounted. mount //server/share [/NET] mounts one.\n");
        return;
    }
    for (int i = 0; i < MAX_MOUNTS; ++i) {
        mount_t *m = &mounts[i];
        if (!m->used)
            continue;
        char how[48] = "disconnected";
        if (smb_alive(m->smb))
            smb_describe(m->smb, how, sizeof how);
        printf("//%s/%s on %s (%s%s%s, %s)\n", m->host, m->share, m->point, m->domain,
               m->domain[0] ? "\\" : "", m->user[0] ? m->user : "guest", how);
    }
}

void smb_cmd_mount(int argc, char **argv) {
    if (argc == 1) {
        list_mounts();
        return;
    }
    const char *target = NULL, *point = "/NET", *who = NULL;
    bool guest = false;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "-u") && i + 1 < argc)
            who = argv[++i];
        else if (!strcmp(argv[i], "-g"))
            guest = true;
        else if (!target)
            target = argv[i];
        else if (argv[i][0] == '/')
            point = argv[i];
        else
            target = NULL, i = argc;
    }
    if (!target || (target[0] != '/' && target[0] != '\\') ||
        (target[1] != '/' && target[1] != '\\')) {
        printf("usage: mount //server[:port]/share [/NET] [-u [domain\\]user] [-g]\n"
               "       mount            (lists what is mounted)\n");
        return;
    }
    if (!net_need_link())
        return;
    if (!mounts && !(mounts = psram_malloc(MAX_MOUNTS * sizeof *mounts))) {
        printf("Out of memory\n");
        return;
    }
    if (!mount_count)
        memset(mounts, 0, MAX_MOUNTS * sizeof *mounts);
    mount_t *m = NULL;
    for (int i = 0; i < MAX_MOUNTS; ++i) {
        if (mounts[i].used && !strcasecmp(mounts[i].point, point)) {
            printf("%s is in use (umount %s first)\n", point, point);
            return;
        }
        if (!mounts[i].used && !m)
            m = &mounts[i];
    }
    if (!m) {
        printf("Too many shares mounted\n");
        return;
    }
    // Split //server[:port]/share.
    char host[64], share[80];
    const char *p = target + 2, *slash = strpbrk(p, "/\\");
    if (!slash || !slash[1]) {
        printf("Give the share as //server/share\n");
        return;
    }
    snprintf(host, sizeof host, "%.*s", (int)(slash - p), p);
    snprintf(share, sizeof share, "%s", slash + 1);
    uint16_t port = 445;
    char *colon = strchr(host, ':');
    if (colon) {
        *colon = 0;
        port = atoi(colon + 1);
    }
    for (char *q = share; *q; ++q)
        if (*q == '/')
            *q = '\\';
    memset(m, 0, sizeof *m);
    char key[160];
    snprintf(key, sizeof key, "//%s/%s", host, share);
    bool from_file = false;
    if (who) {
        const char *bs = strpbrk(who, "\\/");
        if (bs) {
            snprintf(m->domain, sizeof m->domain, "%.*s", (int)(bs - who), who);
            who = bs + 1;
        }
        snprintf(m->user, sizeof m->user, "%s", who);
    } else if (!guest && find_creds(key, m->domain, m->user, m->password)) {
        from_file = true;
        printf("Using the login saved in %s\n", CREDS_FILE);
    } else if (!guest && shell_batch) {
        // A script (/AUTORUN): nobody to type a password at boot.
        printf("mount: no saved login for %s - mount it once by hand and save the login\n", key);
        return;
    } else if (!guest) {
        printf("User (Enter for guest): ");
        if (!read_line(m->user, sizeof m->user, false))
            return;
    }
    bool asked = false;
    if (m->user[0] && !from_file && shell_batch) {
        printf("mount: no saved password for %s - mount it once by hand and save the login\n", key);
        return;
    }
    if (m->user[0] && !from_file) {
        printf("Password for %s: ", m->user);
        if (!read_line(m->password, sizeof m->password, true))
            return;
        asked = true;
    }
    char err[100];
    m->smb = smb_connect(host, port, share, m->domain, m->user, m->password, err, sizeof err);
    if (!m->smb) {
        printf("mount: %s\n", err);
        mbedtls_platform_zeroize(m->password, sizeof m->password);
        return;
    }
    snprintf(m->host, sizeof m->host, "%s", host);
    snprintf(m->share, sizeof m->share, "%s", share);
    snprintf(m->point, sizeof m->point, "%s", point);
    normalise(m->point);
    m->port = port;
    m->used = true;
    ++mount_count;
    char how[48];
    smb_describe(m->smb, how, sizeof how);
    printf("Mounted %s on %s (%s)\n", key, m->point, how);
    if (asked) {
        printf("Save the password in %s? (y/N) ", CREDS_FILE);
        char yn[4];
        if (read_line(yn, sizeof yn, false) && (yn[0] == 'y' || yn[0] == 'Y'))
            save_creds(key, m->domain, m->user, m->password);
    }
}

void smb_cmd_umount(int argc, char **argv) {
    const char *point = argc > 1 ? argv[1] : NULL;
    int found = 0;
    if (!mounts) {
        printf("Nothing is mounted\n");
        return;
    }
    for (int i = 0; i < MAX_MOUNTS; ++i) {
        mount_t *m = &mounts[i];
        if (!m->used || (point && !same_text(m->point, point, strlen(m->point) + 1)))
            continue;
        if (!point && found)
            break;
        for (int k = 0; k < MAX_FILES; ++k)
            if (files[k].fp && files[k].m == m) {
                psram_buffer_free(files[k].cache);
                files[k].fp->obj.fs = NULL;
                files[k] = (typeof(files[0])){0};
            }
        size_t n = strlen(m->point);
        if (vcwd[0] && same_text(vcwd, m->point, n) && (vcwd[n] == 0 || vcwd[n] == '/')) {
            vcwd[0] = 0;
            __real_f_chdir("/");
        }
        smb_disconnect(m->smb);
        printf("Unmounted %s\n", m->point);
        mbedtls_platform_zeroize(m, sizeof *m);
        --mount_count;
        ++found;
    }
    if (!found)
        printf(point ? "%s isn't mounted\n" : "Nothing is mounted\n", point);
}
