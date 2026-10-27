/**
 * @file glob.c
 * @brief Wildcards for the file commands: see glob.h.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */

#include "glob.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ff.h"

bool glob_wild(const char *path) {
    const char *slash = strrchr(path, '/');
    return strpbrk(slash ? slash + 1 : path, "*?") != NULL;
}

static bool match(const char *pat, const char *s) {
    for (; *pat; ++pat, ++s) {
        if (*pat == '*') {
            for (;; ++s) {
                if (match(pat + 1, s))
                    return true;
                if (!*s)
                    return false;
            }
        }
        if (!*s || (*pat != '?' && toupper((unsigned char)*pat) != toupper((unsigned char)*s)))
            return false;
    }
    return !*s;
}

bool glob_match(const char *pattern, const char *name) {
    if (match(pattern, name))
        return true;
    // DOS: "X*.*" also matches X names without an extension.
    size_t n = strlen(pattern);
    if (n >= 2 && !strcmp(pattern + n - 2, ".*") && !strchr(name, '.')) {
        char stem[256];
        snprintf(stem, sizeof stem, "%.*s", (int)(n - 2), pattern);
        return match(stem, name);
    }
    return false;
}

static bool add(char ***list, int *n, int *cap, const char *path) {
    if (*n == *cap) {
        char **bigger = realloc(*list, (*cap + 16) * sizeof **list);
        if (!bigger)
            return false;
        *list = bigger;
        *cap += 16;
    }
    if (!((*list)[*n] = strdup(path)))
        return false;
    ++*n;
    return true;
}

int glob_expand(char *const *args, int count, bool dirs, char ***out) {
    char **list = NULL;
    int n = 0, cap = 0;
    for (int i = 0; i < count; ++i) {
        if (!glob_wild(args[i])) {
            add(&list, &n, &cap, args[i]);
            continue;
        }
        const char *slash = strrchr(args[i], '/');
        const char *pat = slash ? slash + 1 : args[i];
        char folder[256];
        snprintf(folder, sizeof folder, "%.*s", slash ? (int)(slash - args[i]) : 0, args[i]);
        DIR dir;
        FILINFO info;
        int before = n;
        if (f_opendir(&dir, slash && !folder[0] ? "/" : folder) == FR_OK) {
            while (f_readdir(&dir, &info) == FR_OK && info.fname[0]) {
                if ((info.fattrib & (AM_HID | AM_SYS)) || (!dirs && (info.fattrib & AM_DIR)) ||
                    !glob_match(pat, info.fname))
                    continue;
                char path[520];
                snprintf(path, sizeof path, "%s%s%s", folder, slash ? "/" : "", info.fname);
                if (!add(&list, &n, &cap, path))
                    break;
            }
            f_closedir(&dir);
        }
        if (n == before)
            printf("No files match %s\n", args[i]);
    }
    *out = list;
    return n;
}

void glob_free(char **list, int n) {
    for (int i = 0; i < n; ++i)
        free(list[i]);
    free(list);
}

/// An absolute path without . and .. (and any number of /).
void glob_absolute(const char *path, char *out, size_t size) {
    char raw[600] = "";
    if (path[0] != '/' && f_getcwd(raw, 300) != FR_OK)
        strcpy(raw, "/");
    strncat(raw, "/", sizeof raw - strlen(raw) - 1);
    strncat(raw, path, sizeof raw - strlen(raw) - 1);
    size_t n = 0;
    for (const char *p = raw; *p;) {
        while (*p == '/')
            ++p;
        const char *start = p;
        while (*p && *p != '/')
            ++p;
        size_t len = p - start;
        if (!len || (len == 1 && *start == '.'))
            continue;
        if (len == 2 && start[0] == '.' && start[1] == '.') {
            while (n > 0 && out[n - 1] != '/')
                --n;
            if (n > 0)
                --n;
            continue;
        }
        if (n + len + 2 >= size)
            break;
        out[n++] = '/';
        memcpy(out + n, start, len);
        n += len;
    }
    if (!n)
        out[n++] = '/';
    out[n] = 0;
}

bool glob_same(const char *a, const char *b) {
    char x[300], y[300];
    glob_absolute(a, x, sizeof x);
    glob_absolute(b, y, sizeof y);
    return !strcasecmp(x, y);
}

bool glob_inside(const char *inner, const char *outer) {
    char x[300], y[300];
    glob_absolute(inner, x, sizeof x);
    glob_absolute(outer, y, sizeof y);
    size_t n = strlen(y);
    if (n == 1)
        return true;  // everything is inside /
    return !strncasecmp(x, y, n) && (x[n] == 0 || x[n] == '/');
}

int glob_ask(const char *question) {
    printf("%s", question);
    fflush(stdout);
    int c;
    while ((c = getchar_timeout_us(100000)) < 0)
        ;
    c = tolower(c);
    printf("%c\n", c >= 0x20 && c < 0x7f ? c : ' ');
    return c;
}
