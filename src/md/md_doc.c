/**
 * @file md_doc.c
 * @brief Markdown laid out for the md viewer (md_doc.h).
 *
 * Two passes. The first collects the reference links ("[name]: url") so a
 * link may come before its target. The second reads the text a line at a
 * time, as CommonMark does: each line first continues the blocks it is
 * inside (quotes, list items), may open new ones, and what is left is a
 * leaf: part of a paragraph, a heading, code, a table, a rule. A leaf is laid
 * out as soon as it ends, at the current place, so the document grows top to
 * bottom. A paragraph's text is first made "rich" (each character with its
 * style and link: emphasis, code, links and pictures worked out), then
 * wrapped into lines.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */
#include "md_doc.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "psram_heap.h"
#include "www_html.h"

#define LINE_MAX  2048  ///< a line longer than this is cut
#define MAX_DEPTH 16    ///< quotes and list items inside each other
#define MAX_COLS  24    ///< a table's columns
#define GAP       10    ///< between blocks
#define GAP_LIST  4     ///< between blocks in a list item
#define LEADING   2     ///< between lines of text
#define QUOTE_IN  18    ///< a quote's indent
#define BULLET_IN 22    ///< a bulleted item's indent
#define NUMBER_IN 30    ///< a numbered item's indent
#define CELL_PAD  6     ///< inside a table cell, each side
#define CHUNK_H   256   ///< tall rectangles are cut into pieces this tall

/// A rich character's style (RF_).
enum {
    RF_BOLD = 1, RF_ITALIC = 2, RF_CODE = 4, RF_STRIKE = 8,
    RF_HIDE = 16,   ///< markup: dropped when the text is done
    RF_ALT = 32,    ///< a picture's alt text, shown in its place
    RF_BREAK = 64,  ///< a line break (c is '\n')
};

/// A character of a paragraph, with its style.
typedef struct {
    uint8_t c;      ///< CP437 (0: a picture)
    uint8_t f;      ///< RF_
    uint16_t link;  ///< the link it is in, or 0
    uint16_t img;   ///< a picture: imgs index + 1
} rc_t;

/// A block that holds others.
enum { C_QUOTE, C_ITEM };
/// What leaf is open.
enum { L_NONE, L_PARA, L_FENCE, L_ICODE, L_TABLE, L_COMMENT };

/// A quote or a list item.
typedef struct {
    uint8_t type;      ///< C_
    bool ordered;      ///< a numbered item
    bool pending;      ///< its bullet isn't drawn yet
    int8_t task;       ///< a task's box: -1 none, 0 empty, 1 ticked
    char marker;       ///< '-', '*', '+', '.' or ')'
    int col;           ///< where its text starts (in the line)
    int x;             ///< and on the page
    int num;           ///< its number
    int32_t first_y;   ///< where it began to show (quotes), or -1
    int level;         ///< bullets: how many lists it is inside
    bool first;        ///< the first item of its list
} cont_t;

/// The list items just ended at a depth: the next one goes on the same list.
typedef struct {
    bool on;       ///< one just ended
    bool ordered;  ///< numbered
    char marker;   ///< its marker
    int num;       ///< its number
} last_t;

/// A reference link: [label]: url.
typedef struct {
    uint32_t label;  ///< in the pool
    uint32_t url;    ///< in the pool
} ref_t;

/// A line laid out: where, how tall.
typedef struct {
    int32_t y;  ///< its top
    int h;      ///< its height
} ln_t;

/// The builder (one at a time; in PSRAM while md_layout runs).
typedef struct {
    md_doc_t *d;
    int width;
    md_size_fn size_of;
    void *arg;
    rc_t *rc;          ///< the rich text being made
    int nrc, caprc;
    ref_t *refs;       ///< the reference links
    int nrefs, caprefs;
    ln_t *ln;          ///< the lines of the last text laid out
    int nln, capln;
    char *buf;         ///< the open leaf's lines, each ending in '\n'
    uint32_t nbuf, capbuf;
    int nlines;        ///< how many
    int32_t y;         ///< the bottom of what is laid out
    bool started;      ///< something is laid out (blocks after it get a gap)
    cont_t st[MAX_DEPTH];  ///< the blocks open
    int depth;
    last_t last[MAX_DEPTH + 1];  ///< by depth
    int leaf;          ///< L_
    char fence_ch;     ///< a fence's character
    int fence_n;       ///< and how many
    int fence_in;      ///< its indent
    int blanks;        ///< blank lines held back in indented code
    uint8_t align[MAX_COLS];  ///< a table's columns: 0 left, 1 centre, 2 right
    int ncols;
    uint8_t hflags;    ///< HTML's <b>, <i>, <code> on
    bool oom;          ///< ran out of memory
    char l[LINE_MAX + 8];      ///< a line being read
    char cell[LINE_MAX];       ///< a table cell's text
} builder_t;

static builder_t *Bp;  ///< the builder
#define B (*Bp)        ///< it

// ---- Memory ------------------------------------------------------------------------------------------

/// Makes room in a growing array.
/// @param p the array
/// @param cap its room (in elements), raised
/// @param need the elements wanted
/// @param size an element's size
/// @return false if there is no memory (and B.oom is set)
static bool grow(void *p, int *cap, int need, size_t size) {
    void **a = p;
    if (need <= *cap)
        return true;
    if (B.oom)
        return false;
    int n = *cap ? *cap : 64;
    while (n < need)
        n *= 2;
    void *q = psram_realloc(*a, (size_t)n * size);
    if (!q) {
        B.oom = true;
        return false;
    }
    *a = q;
    *cap = n;
    return true;
}

/// Puts text in the pool.
/// @param s it
/// @param n its length
/// @return where (it ends in a 0)
static uint32_t pool_add(const char *s, int n) {
    md_doc_t *d = B.d;
    if (!grow(&d->pool, &d->caps[1], (int)d->npool + n + 1, 1))
        return 0;
    uint32_t at = d->npool;
    memcpy(d->pool + at, s, n);
    d->pool[at + n] = 0;
    d->npool += n + 1;
    return at;
}

/// A new item, after the others.
/// @return it (cleared), or NULL
static md_item_t *item_new(void) {
    md_doc_t *d = B.d;
    if (!grow(&d->items, &d->caps[0], d->nitems + 1, sizeof *d->items))
        return NULL;
    md_item_t *it = &d->items[d->nitems++];
    memset(it, 0, sizeof *it);
    return it;
}

/// A filled rectangle (cut into pieces if tall).
/// @param x left
/// @param y top
/// @param w width
/// @param h height
/// @param c colour
static void rect(int x, int32_t y, int w, int h, uint8_t c) {
    if (x < 0)
        w += x, x = 0;
    while (h > 0 && w > 0) {
        int hh = h > CHUNK_H ? CHUNK_H : h;
        md_item_t *it = item_new();
        if (!it)
            return;
        it->kind = MI_RECT;
        it->x = (int16_t)x, it->y = y, it->w = (uint16_t)w, it->h = (uint16_t)hh;
        it->fg = c;
        y += hh, h -= hh;
    }
}

// ---- Characters -----------------------------------------------------------------------------------

/// Reads a UTF-8 character.
/// @param s where (moved past it)
/// @param end the text's end
/// @return its code point (a bad byte: itself)
static uint32_t utf8(const char **s, const char *end) {
    const uint8_t *p = (const uint8_t *)*s;
    uint32_t u = *p++;
    int more = u >= 0xf0 ? 3 : u >= 0xe0 ? 2 : u >= 0xc0 ? 1 : 0;
    if (u >= 0x80 && u < 0xc0)
        more = 0;
    uint32_t v = more == 3 ? u & 7 : more == 2 ? u & 15 : u & 31;
    const uint8_t *q = p;
    for (int i = 0; i < more; ++i) {
        if ((const char *)q >= end || (*q & 0xc0) != 0x80) {
            *s = (const char *)p;  // not UTF-8: the byte alone (as Latin-1)
            return u;
        }
        v = v << 6 | (*q++ & 0x3f);
    }
    *s = (const char *)q;
    return more ? v : u;
}

/// Adds a rich character.
/// @param c it (CP437)
/// @param f its style
/// @param link its link
static void put(uint8_t c, uint8_t f, uint16_t link) {
    if (!grow(&B.rc, &B.caprc, B.nrc + 1, sizeof *B.rc))
        return;
    B.rc[B.nrc++] = (rc_t){c, (uint8_t)(f | B.hflags), link, 0};
}

/// Adds a code point.
/// @param u it
/// @param f its style
/// @param link its link
static void put_u(uint32_t u, uint8_t f, uint16_t link) {
    if (u == '\t')
        u = ' ';
    uint8_t c = www_cp437(u);
    if (c)
        put(c, f, link);
}

/// Adds text (UTF-8) in one style.
/// @param s it
/// @param n its length
/// @param f its style
/// @param link its link
static void put_text(const char *s, int n, uint8_t f, uint16_t link) {
    const char *end = s + n;
    while (s < end)
        put_u(utf8(&s, end), f, link);
}

/// The named entities known.
static const struct {
    const char *name;  ///< without & and ;
    uint16_t u;        ///< its code point
} entities[] = {
    {"amp", '&'},     {"lt", '<'},       {"gt", '>'},       {"quot", '"'},     {"apos", '\''},
    {"nbsp", 0xa0},   {"copy", 0xa9},    {"reg", 0xae},     {"trade", 0x2122}, {"mdash", 0x2014},
    {"ndash", 0x2013}, {"hellip", 0x2026}, {"middot", 0xb7}, {"deg", 0xb0},     {"plusmn", 0xb1},
    {"times", 0xd7},  {"divide", 0xf7},  {"larr", 0x2190},  {"rarr", 0x2192},  {"uarr", 0x2191},
    {"darr", 0x2193}, {"laquo", 0xab},   {"raquo", 0xbb},   {"lsquo", 0x2018}, {"rsquo", 0x2019},
    {"ldquo", 0x201c}, {"rdquo", 0x201d}, {"bull", 0x2022}, {"euro", 0x20ac},  {"pound", 0xa3},
    {"cent", 0xa2},   {"yen", 0xa5},     {"sect", 0xa7},    {"para", 0xb6},    {"micro", 0xb5},
    {"frac12", 0xbd}, {"frac14", 0xbc},  {"sup2", 0xb2},    {"emsp", ' '},     {"ensp", ' '},
    {"thinsp", ' '},  {"zwj", 0x200d},   {"check", 0x2713}, {"aacute", 0xe1}, {"agrave", 0xe0},
    {"acirc", 0xe2},  {"auml", 0xe4},    {"aring", 0xe5},   {"ccedil", 0xe7},  {"eacute", 0xe9},
    {"egrave", 0xe8}, {"ecirc", 0xea},   {"euml", 0xeb},    {"iacute", 0xed},  {"iuml", 0xef},
    {"ntilde", 0xf1}, {"oacute", 0xf3},  {"ocirc", 0xf4},   {"ouml", 0xf6},    {"uacute", 0xfa},
    {"ugrave", 0xf9}, {"uuml", 0xfc},    {"szlig", 0xdf},   {"Auml", 0xc4},    {"Ouml", 0xd6},
    {"Uuml", 0xdc},   {"Eacute", 0xc9},  {"Ntilde", 0xd1},  {"iexcl", 0xa1},   {"iquest", 0xbf},
};

/// Reads an entity (&name; &#123; &#x1f;).
/// @param s at '&'
/// @param n the characters left
/// @param len set to its length
/// @return its code point, or 0 if it isn't one
static uint32_t entity(const char *s, int n, int *len) {
    int e = 1;
    while (e < n && e < 12 && s[e] != ';' && (isalnum((unsigned char)s[e]) || s[e] == '#'))
        ++e;
    if (e >= n || s[e] != ';' || e < 2)
        return 0;
    *len = e + 1;
    if (s[1] == '#') {
        uint32_t u = s[2] == 'x' || s[2] == 'X' ? strtoul(s + 3, NULL, 16) : strtoul(s + 2, NULL, 10);
        return u ? u : 0xfffd;
    }
    for (unsigned i = 0; i < sizeof entities / sizeof *entities; ++i)
        if ((int)strlen(entities[i].name) == e - 1 && !strncmp(entities[i].name, s + 1, e - 1))
            return entities[i].u;
    return 0;
}

/// Whether a character is Markdown punctuation (escapable, and for emphasis's rules).
/// @param c it
/// @return it is
static bool punct(int c) {
    return c > 0 && c < 0x80 && ispunct(c);
}

/// Whether a character is white space (the start and end of the text count as such).
/// @param c it (0: an end)
/// @return it is
static bool space(int c) {
    return c == 0 || c == ' ' || c == '\t' || c == '\n';
}

// ---- Links -------------------------------------------------------------------------------------------

/// Adds a link.
/// @param url its target
/// @param n its length
/// @return its number (1 on), or 0
static uint16_t link_add(const char *url, int n) {
    md_doc_t *d = B.d;
    if (d->nlinks >= 65535 || !grow(&d->links, &d->caps[2], d->nlinks + 1, sizeof *d->links))
        return 0;
    // Spaces and angle brackets round it go.
    while (n && *url == ' ')
        ++url, --n;
    while (n && url[n - 1] == ' ')
        --n;
    if (n >= 2 && url[0] == '<' && url[n - 1] == '>')
        ++url, n -= 2;
    // Backslash escapes and entities in it are undone.
    char tmp[512];
    int k = 0;
    for (int i = 0; i < n && k < (int)sizeof tmp - 4; ++i) {
        int el;
        uint32_t u;
        if (url[i] == '\\' && i + 1 < n && punct(url[i + 1]))
            tmp[k++] = url[++i];
        else if (url[i] == '&' && (u = entity(url + i, n - i, &el)) && u < 0x80)
            tmp[k++] = (char)u, i += el - 1;
        else
            tmp[k++] = url[i];
    }
    d->links[d->nlinks] = pool_add(tmp, k);
    return (uint16_t)++d->nlinks;
}

/// Compares labels: case and runs of spaces don't matter.
/// @param a one
/// @param an its length
/// @param b another (0-ended)
/// @return they match
static bool label_eq(const char *a, int an, const char *b) {
    const char *ae = a + an;
    while (a < ae && space(*a))
        ++a;
    while (*b && space(*b))
        ++b;
    for (;;) {
        bool sa = false, sb = false;
        while (a < ae && space(*a))
            ++a, sa = true;
        while (*b && space(*b))
            ++b, sb = true;
        bool ea = a >= ae, eb = !*b;
        if (ea || eb)
            return ea && eb;
        if (sa != sb || tolower((unsigned char)*a) != tolower((unsigned char)*b))
            return false;
        ++a, ++b;
    }
}

/// Finds a reference link's target.
/// @param label its label
/// @param n the label's length
/// @return the target (in the pool), or NULL
static const char *ref_find(const char *label, int n) {
    for (int i = 0; i < B.nrefs; ++i)
        if (label_eq(label, n, B.d->pool + B.refs[i].label))
            return B.d->pool + B.refs[i].url;
    return NULL;
}

/// Whether a line is a reference link's definition: [label]: url "title".
/// @param s the line (after the blocks' markers)
/// @param label set to the label (may be NULL)
/// @param ln its length
/// @param url set to the url
/// @param un its length
/// @return it is
static bool ref_def(const char *s, const char **label, int *ln, const char **url, int *un) {
    int i = 0;
    while (s[i] == ' ' && i < 3)
        ++i;
    if (s[i] != '[' || s[i + 1] == '^')
        return false;
    int a = ++i;
    while (s[i] && s[i] != ']' && s[i] != '[')
        i += s[i] == '\\' && s[i + 1] ? 2 : 1;
    if (s[i] != ']' || s[i + 1] != ':' || i == a)
        return false;
    int e = i;
    i += 2;
    while (s[i] == ' ')
        ++i;
    int u = i;
    if (s[i] == '<') {
        while (s[i] && s[i] != '>')
            ++i;
        if (s[i])
            ++i;
    } else {
        while (s[i] && s[i] != ' ' && s[i] != '\n')
            ++i;
    }
    if (i == u)
        return false;
    if (label)
        *label = s + a, *ln = e - a;
    *url = s + u, *un = i - u;
    return true;
}

// ---- Inline text --------------------------------------------------------------------------------

static void inl(const char *s, int n, uint8_t f, uint16_t link);

/// The end of a code span starting at s[i] (a run of backticks), or -1.
/// @param s the text
/// @param n its length
/// @param i where the run starts
/// @param k set to the run's length
/// @return where the closing run starts, or -1
static int code_end(const char *s, int n, int i, int *k) {
    int r = 0;
    while (i + r < n && s[i + r] == '`')
        ++r;
    *k = r;
    for (int j = i + r; j < n;) {
        if (s[j] != '`') {
            ++j;
            continue;
        }
        int q = 0;
        while (j + q < n && s[j + q] == '`')
            ++q;
        if (q == r)
            return j;
        j += q;
    }
    return -1;
}

/// A picture: its size asked for, then put in (or its alt text, if it can't be shown).
/// @param src where it is
/// @param sn its length
/// @param alt its alt text
/// @param an its length
/// @param want_w the width the document asks for (0: its own)
/// @param f the style round it
/// @param link the link round it
static void picture(const char *src, int sn, const char *alt, int an, int want_w, uint8_t f, uint16_t link) {
    md_doc_t *d = B.d;
    while (sn && *src == ' ')
        ++src, --sn;
    while (sn && src[sn - 1] == ' ')
        --sn;
    if (sn >= 2 && src[0] == '<' && src[sn - 1] == '>')
        ++src, sn -= 2;
    char path[400];
    snprintf(path, sizeof path, "%.*s", sn, src);
    int w = 0, h = 0;
    if (B.size_of && sn && d->nimgs < 65535 && B.size_of(B.arg, path, &w, &h) && w > 0 && h > 0 &&
        grow(&d->imgs, &d->caps[3], d->nimgs + 1, sizeof *d->imgs)) {
        if (want_w > 0 && want_w != w) {
            h = (int)((int64_t)h * want_w / w);
            w = want_w;
        }
        md_img_t *im = &d->imgs[d->nimgs++];
        im->w = (uint16_t)(w > 65535 ? 65535 : w);
        im->h = (uint16_t)(h > 65535 ? 65535 : h ? h : 1);
        im->src = pool_add(path, (int)strlen(path));
        im->alt = pool_add(alt, an);
        if (grow(&B.rc, &B.caprc, B.nrc + 1, sizeof *B.rc))
            B.rc[B.nrc++] = (rc_t){0, f, link, (uint16_t)d->nimgs};
        return;
    }
    // Its alt text instead.
    put('[', f | RF_ALT, link);
    if (an)
        put_text(alt, an, f | RF_ALT, link);
    else
        put_text("picture", 7, f | RF_ALT, link);
    put(']', f | RF_ALT, link);
}

/// A picture's size, as the document wants it.
/// @param img its imgs index
/// @param w set to its width
/// @param h set to its height
static void picture_size(int img, int *w, int *h) {
    *w = B.d->imgs[img].w;
    *h = B.d->imgs[img].h;
}

/// A link or picture at s[*i] ('[' or "!["): put in, and *i moved past it.
/// @param s the text
/// @param n its length
/// @param i where (moved)
/// @param f the style round it
/// @param link the link round it
/// @param img a picture
/// @return it was one (else nothing is done)
static bool link_at(const char *s, int n, int *i, uint8_t f, uint16_t link, bool img) {
    int a = *i + (img ? 2 : 1), j = a, depth = 1;
    while (j < n) {  // the matching ]
        if (s[j] == '\\' && j + 1 < n) {
            j += 2;
            continue;
        }
        if (s[j] == '`') {
            int k, e = code_end(s, n, j, &k);
            j = e >= 0 ? e + k : j + k;
            continue;
        }
        if (s[j] == '[')
            ++depth;
        else if (s[j] == ']' && !--depth)
            break;
        ++j;
    }
    if (j >= n)
        return false;
    const char *text = s + a;
    int tn = j - a, k = j + 1, end;
    const char *url = NULL;
    int un = 0;
    if (k < n && s[k] == '(') {
        int p = k + 1;
        while (p < n && (s[p] == ' ' || s[p] == '\n'))
            ++p;
        int u = p, parens = 0;
        if (p < n && s[p] == '<') {
            while (p < n && s[p] != '>' && s[p] != '\n')
                ++p;
            if (p >= n || s[p] != '>')
                return false;
            ++p;
        } else {
            while (p < n && s[p] != ' ' && s[p] != '\n') {
                if (s[p] == '\\' && p + 1 < n)
                    ++p;
                else if (s[p] == '(')
                    ++parens;
                else if (s[p] == ')' && !parens--)
                    break;
                ++p;
            }
        }
        url = s + u, un = p - u;
        while (p < n && (s[p] == ' ' || s[p] == '\n'))
            ++p;
        if (p < n && (s[p] == '"' || s[p] == '\'' || s[p] == '(')) {  // a title: skipped
            char close = s[p] == '(' ? ')' : s[p];
            ++p;
            while (p < n && s[p] != close)
                p += s[p] == '\\' ? 2 : 1;
            ++p;
            while (p < n && (s[p] == ' ' || s[p] == '\n'))
                ++p;
        }
        if (p >= n || s[p] != ')')
            return false;
        end = p + 1;
    } else if (k < n && s[k] == '[') {
        int p = k + 1;
        while (p < n && s[p] != ']')
            ++p;
        if (p >= n)
            return false;
        url = p == k + 1 ? ref_find(text, tn) : ref_find(s + k + 1, p - k - 1);
        if (!url)
            return false;
        un = (int)strlen(url);
        end = p + 1;
    } else {
        if (!(url = ref_find(text, tn)))
            return false;
        un = (int)strlen(url);
        end = k;
    }
    if (img) {
        // The alt text, without markup.
        char alt[200];
        int an = 0;
        for (int p = 0; p < tn && an < (int)sizeof alt - 1; ++p)
            if (!strchr("*_`[]", text[p]) && text[p] != '\n')
                alt[an++] = text[p];
        picture(url, un, alt, an, 0, f, link);
    } else {
        uint16_t id = link ? link : link_add(url, un);
        int from = B.nrc;
        inl(text, tn, f, id);
        for (int p = from; p < B.nrc; ++p)
            B.rc[p].link = id;
    }
    *i = end;
    return true;
}

/// An HTML attribute's value.
/// @param tag the tag
/// @param n its length
/// @param name the attribute
/// @param vn set to the value's length
/// @return the value, or NULL
static const char *attr(const char *tag, int n, const char *name, int *vn) {
    int l = (int)strlen(name);
    for (int i = 1; i + l < n; ++i) {
        if (strncasecmp(tag + i, name, l) || !space(tag[i - 1]) || tag[i + l] != '=')
            continue;
        int p = i + l + 1;
        char q = tag[p] == '"' || tag[p] == '\'' ? tag[p++] : 0;
        int v = p;
        while (p < n && (q ? tag[p] != q : !space(tag[p]) && tag[p] != '>' && tag[p] != '/'))
            ++p;
        *vn = p - v;
        return tag + v;
    }
    return NULL;
}

/// An HTML tag at s[*i] ('<'): <br> breaks the line, <img> is a picture, <b> <i> <code> and the like
/// style the text, others are dropped; an autolink <http://...> is a link. *i is moved past it.
/// @param s the text
/// @param n its length
/// @param i where (moved)
/// @param f the style round it
/// @param link the link round it
/// @return it was a tag or autolink (else '<' is text)
static bool tag_at(const char *s, int n, int *i, uint8_t f, uint16_t link) {
    int a = *i + 1, j = a;
    // An autolink: <scheme:...> or <user@host>.
    while (j < n && s[j] != '>' && s[j] != '<' && s[j] != ' ' && s[j] != '\n')
        ++j;
    if (j < n && s[j] == '>' && j > a && (memchr(s + a, ':', j - a) || memchr(s + a, '@', j - a)) &&
        isalpha((unsigned char)s[a])) {
        bool mail = !memchr(s + a, ':', j - a);
        char url[400];
        snprintf(url, sizeof url, "%s%.*s", mail ? "mailto:" : "", j - a, s + a);
        uint16_t id = link ? link : link_add(url, (int)strlen(url));
        put_text(s + a, j - a, f, id);
        *i = j + 1;
        return true;
    }
    // A tag: <name ...>, </name>, <!-- -->.
    if (a + 3 <= n && !strncmp(s + a, "!--", 3)) {
        for (j = a + 3; j + 2 < n; ++j)
            if (!strncmp(s + j, "-->", 3)) {
                *i = j + 3;
                return true;
            }
        return false;
    }
    bool close = a < n && s[a] == '/';
    int b = a + close;
    if (b >= n || !isalpha((unsigned char)s[b]))
        return false;
    j = b;
    while (j < n && (isalnum((unsigned char)s[j]) || s[j] == '-'))
        ++j;
    char name[12];
    snprintf(name, sizeof name, "%.*s", j - b, s + b);
    char q = 0;
    for (; j < n && (q || s[j] != '>'); ++j)
        if (q ? s[j] == q : s[j] == '"' || s[j] == '\'')
            q = q ? 0 : s[j];
    if (j >= n)
        return false;
    const char *tag = s + *i;
    int tn = j + 1 - *i;
    *i = j + 1;
    uint8_t bit = !strcasecmp(name, "b") || !strcasecmp(name, "strong") ? RF_BOLD
                  : !strcasecmp(name, "i") || !strcasecmp(name, "em") ? RF_ITALIC
                  : !strcasecmp(name, "code") || !strcasecmp(name, "kbd") || !strcasecmp(name, "tt") ? RF_CODE
                  : !strcasecmp(name, "s") || !strcasecmp(name, "del") || !strcasecmp(name, "strike") ? RF_STRIKE
                  : 0;
    if (bit) {
        B.hflags = close ? B.hflags & ~bit : B.hflags | bit;
    } else if (!close && !strcasecmp(name, "br")) {
        put('\n', f | RF_BREAK, link);
    } else if (!close && !strcasecmp(name, "img")) {
        int sn = 0, an = 0, wn = 0;
        const char *src = attr(tag, tn, "src", &sn), *alt = attr(tag, tn, "alt", &an);
        const char *w = attr(tag, tn, "width", &wn);
        if (src)
            picture(src, sn, alt ? alt : "", alt ? an : 0, w && !memchr(w, '%', wn) ? atoi(w) : 0, f, link);
    } else if (!close && !strcasecmp(name, "a")) {
        // <a href>: the text up to </a> is the link.
        int hn = 0;
        const char *href = attr(tag, tn, "href", &hn);
        int e = *i;
        while (e + 3 < n && strncasecmp(s + e, "</a>", 4))
            ++e;
        if (href && e + 3 < n) {
            uint16_t id = link ? link : link_add(href, hn);
            int from = B.nrc;
            inl(s + *i, e - *i, f, id);
            for (int p = from; p < B.nrc; ++p)
                B.rc[p].link = id;
            *i = e + 4;
        }
    }
    return true;
}

/// An emphasis run waiting for its close.
typedef struct {
    char c;   ///< '*', '_' or '~'
    int pos;  ///< where its characters (still unused) start in B.rc
    int cnt;  ///< how many are left
} delim_t;

/// Makes text rich: appends it to B.rc with its styles and links worked out (markup characters are
/// marked RF_HIDE, to be dropped by finish()).
/// @param s the text
/// @param n its length
/// @param f the style round it
/// @param link the link round it
static void inl(const char *s, int n, uint8_t f, uint16_t link) {
    delim_t ds[32];
    int nds = 0;
    for (int i = 0; i < n && !B.oom;) {
        char c = s[i];
        int prev = i ? (unsigned char)s[i - 1] : 0;
        if (c == '\\' && i + 1 < n && punct(s[i + 1])) {
            put((uint8_t)s[i + 1], f, link);
            i += 2;
        } else if (c == '\n' || (c == '\\' && i + 1 < n && s[i + 1] == '\n')) {
            // A line break if the line ends in two spaces or a backslash, else a space.
            int sp = 0;
            while (B.nrc && B.rc[B.nrc - 1].c == ' ' && !(B.rc[B.nrc - 1].f & RF_CODE))
                --B.nrc, ++sp;
            bool hard = c == '\\' || sp >= 2;
            put(hard ? '\n' : ' ', hard ? f | RF_BREAK : f, link);
            i += c == '\\' ? 2 : 1;
            while (i < n && s[i] == ' ')
                ++i;
        } else if (c == '`') {
            int k, e = code_end(s, n, i, &k);
            if (e < 0) {
                for (int q = 0; q < k; ++q)
                    put('`', f, link);
                i += k;
                continue;
            }
            int a = i + k, b = e;
            bool all = true;
            for (int q = a; q < b; ++q)
                all &= s[q] == ' ' || s[q] == '\n';
            if (!all && b - a >= 2 && (s[a] == ' ' || s[a] == '\n') && (s[b - 1] == ' ' || s[b - 1] == '\n'))
                ++a, --b;
            for (const char *p = s + a, *pe = s + b; p < pe;) {
                uint32_t u = utf8(&p, pe);
                put_u(u == '\n' ? ' ' : u, f | RF_CODE, link);
            }
            i = e + k;
        } else if (c == '!' && i + 1 < n && s[i + 1] == '[' && link_at(s, n, &i, f, link, true)) {
            continue;
        } else if (c == '[' && link_at(s, n, &i, f, link, false)) {
            continue;
        } else if (c == '<' && tag_at(s, n, &i, f, link)) {
            continue;
        } else if (c == '&') {
            int el = 0;
            uint32_t u = entity(s + i, n - i, &el);
            if (u) {
                put_u(u, f, link);
                i += el;
            } else {
                put('&', f, link);
                ++i;
            }
        } else if (c == '*' || c == '_' || c == '~') {
            int k = 0;
            while (i + k < n && s[i + k] == c)
                ++k;
            int next = i + k < n ? (unsigned char)s[i + k] : 0;
            bool left = !space(next) && (!punct(next) || space(prev) || punct(prev));
            bool right = !space(prev) && (!punct(prev) || space(next) || punct(next));
            bool can_open = left, can_close = right;
            if (c == '_') {
                can_open = left && (!right || punct(prev));
                can_close = right && (!left || punct(next));
            }
            if (c == '~' && k != 2)
                can_open = can_close = false;
            int at = B.nrc;
            for (int q = 0; q < k; ++q)
                put((uint8_t)c, f, link);
            int left_n = k, used = 0;
            while (can_close && left_n > 0) {
                int j = nds - 1;
                while (j >= 0 && ds[j].c != c)
                    --j;
                if (j < 0)
                    break;
                delim_t *o = &ds[j];
                int use = c == '~' ? 2 : (left_n >= 2 && o->cnt >= 2 ? 2 : 1);
                uint8_t bit = c == '~' ? RF_STRIKE : use == 2 ? RF_BOLD : RF_ITALIC;
                for (int q = o->pos + o->cnt - use; q < o->pos + o->cnt; ++q)
                    B.rc[q].f |= RF_HIDE;
                for (int q = at + used; q < at + used + use && q < B.nrc; ++q)
                    B.rc[q].f |= RF_HIDE;
                for (int q = o->pos + o->cnt; q < at + used && q < B.nrc; ++q)
                    B.rc[q].f |= bit;
                o->cnt -= use;
                left_n -= use, used += use;
                nds = o->cnt ? j + 1 : j;
            }
            if (left_n > 0 && can_open && nds < 32)
                ds[nds++] = (delim_t){c, at + used, left_n};
            i += k;
        } else if (c == 'h' && !link && (prev == 0 || !isalnum(prev)) &&
                   ((n - i > 7 && !strncmp(s + i, "http://", 7)) || (n - i > 8 && !strncmp(s + i, "https://", 8)))) {
            // A bare web address.
            int e = i;
            while (e < n && !space((unsigned char)s[e]) && s[e] != '<')
                ++e;
            while (e > i && strchr(".,;:!?)\"'*_", s[e - 1]))
                --e;
            uint16_t id = link_add(s + i, e - i);
            put_text(s + i, e - i, f, id);
            i = e;
        } else {
            const char *p = s + i;
            put_u(utf8(&p, s + n), f, link);
            i = (int)(p - s);
        }
    }
}

/// Makes a paragraph's text rich: B.rc is set to it, markup dropped.
/// @param s the text
/// @param n its length
static void rich(const char *s, int n) {
    B.nrc = 0;
    B.hflags = 0;
    while (n && (s[n - 1] == '\n' || s[n - 1] == ' '))
        --n;
    inl(s, n, 0, 0);
    int k = 0;
    for (int i = 0; i < B.nrc; ++i)
        if (!(B.rc[i].f & RF_HIDE))
            B.rc[k++] = B.rc[i];
    B.nrc = k;
}

// ---- Wrapping ------------------------------------------------------------------------------------

static const md_widths_t *Widths;  ///< proportional widths (md_set_widths), or NULL

void md_set_widths(const md_widths_t *w) {
    Widths = w;
}

/// A character's width.
/// @param c the character (CP437)
/// @param font MD_FONT_
/// @param fl its MF_ flags
/// @return its width in pixels
static int char_w(uint8_t c, int font, uint8_t fl) {
    if (!Widths || (fl & (MF_CODE | MF_MONO)))
        return MD_CW(font);
    return Widths->adv[font][(fl & MF_BOLD ? 1 : 0) | (fl & MF_ITALIC ? 2 : 0)][c];
}

int md_char_w(const md_item_t *it, uint8_t c) {
    return char_w(c, it->font, it->flags);
}

/// A rich character's width.
/// @param r it
/// @param font MD_FONT_
/// @param mf MF_ flags for all of the text
/// @return its width in pixels
static int rc_w(const rc_t *r, int font, uint8_t mf) {
    uint8_t f = mf;
    f |= r->f & RF_BOLD ? MF_BOLD : 0;
    f |= r->f & RF_ITALIC ? MF_ITALIC : 0;
    f |= r->f & RF_CODE ? MF_CODE : 0;
    return char_w(r->c, font, f);
}

/// Text laid out into lines: items from the rich characters r[0..n), in lines no wider than w, from (x, y).
/// Each line's place goes in B.ln.
/// @param r the text
/// @param n its length
/// @param x its left
/// @param w its width
/// @param y its top
/// @param font MD_FONT_
/// @param fg its colour
/// @param mf MF_ flags for all of it
/// @param align 0 left, 1 centre, 2 right
/// @return the bottom
static int32_t flow(const rc_t *r, int n, int x, int w, int32_t y, int font, uint8_t fg, uint8_t mf, int align) {
    md_doc_t *d = B.d;
    int cw = MD_CW(font), ch = MD_CH(font);
    B.nln = 0;
    if (w < cw)
        w = cw;
    int i = 0;
    while (i < n || B.nln == 0) {
        int line_first = d->nitems, lx = 0, lh = ch;
        while (i < n && r[i].c == ' ' && !(r[i].f & RF_CODE))  // spaces at a line's start go
            ++i;
        md_item_t *run = NULL;  // the item being added to
        while (i < n) {
            if (r[i].f & RF_BREAK) {
                ++i;
                break;
            }
            // The next word (or picture), and its width.
            int e = i, ww = 0;
            if (r[i].c == 0 && r[i].img) {
                int pw, ph;
                picture_size(r[i].img - 1, &pw, &ph);
                e = i + 1, ww = pw > w ? w : pw;
            } else if (r[i].c == ' ') {
                while (e < n && r[e].c == ' ' && !(r[e].f & RF_BREAK))
                    ww += rc_w(&r[e++], font, mf);
            } else {
                while (e < n && r[e].c != ' ' && r[e].c != 0 && !(r[e].f & RF_BREAK))
                    ww += rc_w(&r[e++], font, mf);
            }
            if (lx + ww > w && lx > 0) {
                if (r[i].c == ' ')  // spaces at the end of a line go too
                    i = e;
                break;
            }
            if (r[i].c == 0) {  // a picture
                int pw, ph;
                picture_size(r[i].img - 1, &pw, &ph);
                if (pw > w)
                    ph = (int)((int64_t)ph * w / pw), pw = w;
                if (ph > MD_PIC_MAX_H)
                    pw = (int)((int64_t)pw * MD_PIC_MAX_H / ph), ph = MD_PIC_MAX_H;
                if (pw < 1)
                    pw = 1;
                if (ph < 1)
                    ph = 1;
                md_item_t *it = item_new();
                if (!it)
                    return y;
                it->kind = MI_IMG;
                it->x = (int16_t)(x + lx), it->w = (uint16_t)pw, it->h = (uint16_t)ph;
                it->ref = r[i].img - 1;
                it->link = r[i].link;
                lx += pw;
                if (ph > lh)
                    lh = ph;
                run = NULL;
                ++i;
                continue;
            }
            // A word too long for a line on its own is cut.
            if (ww > w - lx) {
                int fit = 0, k = i;
                while (k < e && fit + rc_w(&r[k], font, mf) <= w - lx)
                    fit += rc_w(&r[k++], font, mf);
                e = k > i ? k : i + 1;
            }
            for (; i < e; ++i) {
                const rc_t *c = &r[i];
                uint8_t f = mf;
                f |= c->f & RF_BOLD ? MF_BOLD : 0;
                f |= c->f & RF_ITALIC ? MF_ITALIC : 0;
                f |= c->f & RF_STRIKE ? MF_STRIKE : 0;
                f |= c->link ? MF_ULINE : 0;
                f |= c->f & RF_CODE ? MF_CODE | MF_BG : 0;
                uint8_t col = c->link ? MD_C_LINK
                              : c->f & RF_CODE ? MD_C_CODE
                              : c->f & RF_ALT ? MD_C_ALT
                              : (c->f & RF_BOLD) && fg == MD_C_TEXT ? MD_C_BOLD
                                                                     : fg;
                if (!run || run->flags != f || run->fg != col || run->link != c->link || run->len >= 1000) {
                    if (!(run = item_new()))
                        return y;
                    run->kind = MI_TEXT;
                    run->x = (int16_t)(x + lx);
                    run->h = (uint16_t)ch;
                    run->font = (uint8_t)font;
                    run->flags = f;
                    run->fg = col;
                    run->bg = MD_C_CODE_BG;
                    run->link = c->link;
                    run->ref = d->npool;
                }
                if (!grow(&d->pool, &d->caps[1], (int)d->npool + 2, 1))
                    return y;
                d->pool[d->npool++] = (char)c->c;
                d->pool[d->npool] = 0;
                run->len++;
                int cwi = char_w(c->c, font, f);
                run->w = (uint16_t)(run->w + cwi);
                lx += cwi;
            }
        }
        // Spaces at the end come off.
        for (int k = d->nitems - 1; k >= line_first; --k) {
            md_item_t *it = &d->items[k];
            if (it->kind != MI_TEXT)
                break;
            int sw = char_w(' ', it->font, it->flags);
            while (it->len && d->pool[it->ref + it->len - 1] == ' ')
                it->len--, it->w = (uint16_t)(it->w - sw), lx -= sw;
            if (it->len)
                break;
            d->nitems--;  // (only ever the last)
        }
        int shift = align == 1 ? (w - lx) / 2 : align == 2 ? w - lx : 0;
        for (int k = line_first; k < d->nitems; ++k) {
            md_item_t *it = &d->items[k];
            it->y = y + lh - it->h;  // on the line's foot
            it->x = (int16_t)(it->x + (shift > 0 ? shift : 0));
        }
        if (grow(&B.ln, &B.capln, B.nln + 1, sizeof *B.ln))
            B.ln[B.nln++] = (ln_t){y, lh};
        y += lh + LEADING;
        if (B.oom)
            break;
    }
    return y - LEADING;
}

// ---- Blocks --------------------------------------------------------------------------------------

/// The left of the innermost block.
/// @return it
static int left(void) {
    return B.depth ? B.st[B.depth - 1].x : 0;
}

/// Space before a block.
/// @param extra more than usual
static void gap(int extra) {
    if (B.started) {
        // A list item's blocks are close together, except before a list that isn't in another.
        int g = GAP;
        if (B.depth && B.st[B.depth - 1].type == C_ITEM) {
            const cont_t *t = &B.st[B.depth - 1];
            bool outer = B.depth < 2 || B.st[B.depth - 2].type != C_ITEM;
            g = t->pending && t->first && outer ? GAP : GAP_LIST;
        }
        g += extra;
        // Quotes that already show carry on through it.
        for (int i = 0; i < B.depth; ++i)
            if (B.st[i].type == C_QUOTE && B.st[i].first_y >= 0)
                rect(B.st[i].x - QUOTE_IN + 4, B.y, 3, g, MD_C_BAR);
        B.y += g;
    }
    B.started = true;
}

/// After a block from top to bottom: the bars of the quotes it is in, and the bullets of list items it
/// starts.
/// @param top the block's top
/// @param first_h its first line's height (for bullets)
static void decorate(int32_t top, int first_h) {
    for (int i = 0; i < B.depth; ++i) {
        cont_t *c = &B.st[i];
        if (c->type == C_QUOTE) {
            if (c->first_y < 0)
                c->first_y = top;
            rect(c->x - QUOTE_IN + 4, top, 3, (int)(B.y - top), MD_C_BAR);
            continue;
        }
        if (!c->pending)
            continue;
        c->pending = false;
        int in = BULLET_IN;
        int32_t by = top + first_h - 16;  // on the first line's foot
        char t[16];
        int n;
        if (c->task >= 0) {
            int bx = c->x - in + 4 < 0 ? 0 : c->x - in + 4;
            rect(bx, by + 3, 11, 1, MD_C_BULLET);
            rect(bx, by + 13, 11, 1, MD_C_BULLET);
            rect(bx, by + 3, 1, 11, MD_C_BULLET);
            rect(bx + 10, by + 3, 1, 11, MD_C_BULLET);
            if (!c->task)
                continue;
            t[0] = (char)0xfb, n = 1;  // a tick
        } else if (c->ordered) {
            n = snprintf(t, sizeof t, "%d%c", c->num, c->marker);
        } else {
            static const uint8_t bullets[] = {0x07, 0x09, 0xfe};
            t[0] = (char)bullets[c->level % 3], n = 1;
        }
        md_item_t *it = item_new();
        if (!it)
            return;
        it->kind = MI_TEXT;
        it->font = MD_FONT_8x16;
        it->fg = c->task > 0 ? MD_C_TICK : MD_C_BULLET;
        it->flags = (c->task > 0 ? MF_BOLD : 0) | MF_MONO;
        it->len = (uint16_t)n;
        it->w = (uint16_t)(n * 8);
        it->h = 16;
        it->ref = pool_add(t, n);
        it->y = by;
        int bx = c->task >= 0 ? c->x - in + 5 : c->ordered ? c->x - 6 - n * 8 : c->x - in + 7;
        it->x = (int16_t)(bx < 0 ? 0 : bx);
    }
}

/// The colour of text in the blocks open (grey in a quote).
/// @return it
static uint8_t text_colour(void) {
    for (int i = 0; i < B.depth; ++i)
        if (B.st[i].type == C_QUOTE)
            return MD_C_QUOTE;
    return MD_C_TEXT;
}

/// Lays out a paragraph or a heading.
/// @param s its text
/// @param n its length
/// @param level 0 a paragraph, else the heading's level
static void emit_text(const char *s, int n, int level) {
    if (!level) {  // reference definitions at its start are not text
        const char *l, *u;
        int ln, un;
        while (n > 0 && ref_def(s, &l, &ln, &u, &un)) {
            const char *nl = memchr(s, '\n', n);
            if (!nl)
                return;
            n -= (int)(nl + 1 - s);
            s = nl + 1;
        }
        if (n <= 0)
            return;
    }
    rich(s, n);
    if (!B.nrc && !level)
        return;
    static const uint8_t colours[] = {MD_C_H1, MD_C_H2, MD_C_H3, MD_C_H4, MD_C_H4, MD_C_H4};
    int font = level == 1 ? MD_FONT_16x32 : level == 2 ? MD_FONT_12x24 : MD_FONT_8x16;
    gap(level == 1 || level == 2 ? 8 : level ? 4 : 0);
    int32_t top = B.y;
    int x = left(), w = B.width - x;
    B.y = flow(B.rc, B.nrc, x, w, B.y, font, level ? colours[level - 1] : text_colour(),
               level >= 3 ? MF_BOLD : level ? 0 : 0, 0);
    int first_h = B.nln ? B.ln[0].h : 16;
    if (level == 1 || level == 2) {  // a rule under it
        B.y += 3;
        rect(x, B.y, w, 1, MD_C_RULE);
        B.y += 1;
    }
    decorate(top, first_h);
    if (level) {
        md_doc_t *d = B.d;
        if (!grow(&d->heads, &d->caps[4], d->nheads + 1, sizeof *d->heads))
            return;
        char t[120], slug[120];
        int tn = 0, sn = 0;
        for (int i = 0; i < B.nrc && tn < (int)sizeof t - 1; ++i)
            if (B.rc[i].c)
                t[tn++] = (char)B.rc[i].c;
        for (int i = 0; i < tn && sn < (int)sizeof slug - 1; ++i) {
            uint8_t c = (uint8_t)t[i];
            if (isalnum(c) || c == '-' || c == '_')
                slug[sn++] = (char)tolower(c);
            else if (c == ' ')
                slug[sn++] = '-';
        }
        md_head_t *h = &d->heads[d->nheads++];
        h->y = top;
        h->level = (uint8_t)level;
        h->text = pool_add(t, tn);
        h->slug = pool_add(slug, sn);
    }
}

/// Lays out code: each line as it is, cut where it is too long, on a dark band.
/// @param s the lines
/// @param n their length
/// @param band the band behind (else plain text: the source view)
static void emit_code(const char *s, int n, bool band) {
    md_doc_t *d = B.d;
    gap(0);
    int32_t top = B.y;
    int x = left(), w = B.width - x, cols = (w - (band ? 16 : 0)) / 8;
    if (cols < 1)
        cols = 1;
    if (band) {
        rect(x, B.y, w, 6, MD_C_CODE_BG);
        B.y += 6;
    }
    const char *end = s + n;
    while (s < end && !B.oom) {
        const char *nl = memchr(s, '\n', end - s);
        const char *le = nl ? nl : end;
        // The line in CP437, then in pieces of cols characters.
        int k = 0;
        bool any = false;
        for (const char *p = s; p < le || !any;) {
            any = true;
            if (band)
                rect(x, B.y, w, 18, MD_C_CODE_BG);
            md_item_t *it = item_new();
            if (!it || !grow(&d->pool, &d->caps[1], (int)d->npool + 4 * cols + 2, 1))
                return;
            it->kind = MI_TEXT;
            it->font = MD_FONT_8x16;
            it->fg = band ? MD_C_CODE : MD_C_TEXT;
            it->flags = band ? MF_CODE | MF_MONO : MF_MONO;
            it->x = (int16_t)(x + (band ? 8 : 0));
            it->y = B.y + 1;
            it->h = 16;
            it->ref = d->npool;
            for (k = 0; k < cols && p < le;) {
                uint32_t u = utf8(&p, le);
                uint8_t c = www_cp437(u == '\t' ? ' ' : u);
                if (!c)
                    continue;
                d->pool[d->npool++] = (char)c;
                ++k;
            }
            d->pool[d->npool++] = 0;
            it->len = (uint16_t)k;
            it->w = (uint16_t)(k * 8);
            B.y += 18;
        }
        s = nl ? nl + 1 : end;
    }
    if (band) {
        rect(x, B.y, w, 6, MD_C_CODE_BG);
        B.y += 6;
    }
    decorate(top, band ? 24 : 18);
}

/// Lays out a rule.
static void emit_rule(void) {
    gap(0);
    int32_t top = B.y;
    rect(left(), B.y + 7, B.width - left(), 2, MD_C_RULE);
    B.y += 16;
    decorate(top, 16);
}

/// A table row's cells.
/// @param s the row
/// @param n its length
/// @param a set to each cell's start
/// @param b set to each cell's end
/// @param max room in a and b
/// @return how many
static int cells(const char *s, int n, int *a, int *b, int max) {
    int i = 0, k = 0;
    while (i < n && s[i] == ' ')
        ++i;
    if (i < n && s[i] == '|')
        ++i;
    while (n > i && (s[n - 1] == ' ' || s[n - 1] == '\n'))
        --n;
    if (n > i && s[n - 1] == '|' && (n < 2 || s[n - 2] != '\\'))
        --n;
    while (i <= n && k < max) {
        int j = i;
        bool code = false;
        while (j < n && (s[j] != '|' || code)) {
            if (s[j] == '\\' && j + 1 < n)
                ++j;
            else if (s[j] == '`')
                code = !code;
            ++j;
        }
        int x = i, y = j;
        while (x < y && s[x] == ' ')
            ++x;
        while (y > x && s[y - 1] == ' ')
            --y;
        a[k] = x, b[k] = y, ++k;
        i = j + 1;
        if (j >= n)
            break;
    }
    return k;
}

/// Whether a line is a table's delimiter row (|---|:--:|), and its columns' alignment.
/// @param s the line
/// @param align set (MAX_COLS)
/// @return its columns, or 0 if it isn't one
static int delim_row(const char *s, uint8_t *align) {
    int n = (int)strlen(s);
    int a[MAX_COLS], b[MAX_COLS], k = cells(s, n, a, b, MAX_COLS);
    if (!strchr(s, '|') && k < 2)
        return 0;
    for (int i = 0; i < k; ++i) {
        int x = a[i], y = b[i];
        bool l = x < y && s[x] == ':', r = y > x && s[y - 1] == ':';
        x += l, y -= r;
        if (y <= x)
            return 0;
        for (int j = x; j < y; ++j)
            if (s[j] != '-')
                return 0;
        align[i] = l && r ? 1 : r ? 2 : 0;
    }
    return k;
}

/// Lays out a table: the header row then the others, columns as wide as their text (narrowed to fit,
/// the text then wrapped).
/// @param s its rows (the delimiter row left out)
/// @param n their length
static void emit_table(const char *s, int n) {
    int nc = B.ncols, x0 = left(), avail = B.width - x0 - 1;
    // The rows.
    int rows = 0;
    for (int i = 0; i < n; ++i)
        rows += s[i] == '\n';
    if (!rows)
        return;
    int *cell = psram_malloc(sizeof(int) * 2 * rows * nc);  // each cell's rich text: start, length
    if (!cell) {
        B.oom = true;
        return;
    }
    // Every cell made rich, one after the other in B.rc (rich() starts it again, so keep a copy).
    rc_t *all = NULL;
    int nall = 0, capall = 0;
    int nat[MAX_COLS] = {0};
    const char *p = s;
    for (int r = 0; r < rows; ++r) {
        const char *nl = memchr(p, '\n', s + n - p);
        int a[MAX_COLS], b[MAX_COLS], k = cells(p, (int)(nl - p), a, b, nc);
        for (int c = 0; c < nc; ++c) {
            int *ce = &cell[(r * nc + c) * 2];
            ce[0] = nall, ce[1] = 0;
            if (c >= k)
                continue;
            // A cell's \| is a |.
            char *tmp = B.cell;
            int tn = 0;
            for (int i = a[c]; i < b[c] && tn < LINE_MAX - 1; ++i) {
                if (p[i] == '\\' && i + 1 < b[c] && p[i + 1] == '|')
                    ++i;
                tmp[tn++] = p[i];
            }
            rich(tmp, tn);
            if (!grow(&all, &capall, nall + B.nrc, sizeof *all))
                break;
            memcpy(all + nall, B.rc, B.nrc * sizeof *all);
            ce[1] = B.nrc;
            nall += B.nrc;
            int wd = 0;
            for (int i = 0; i < B.nrc; ++i) {
                if (B.rc[i].c == 0) {
                    int pw, ph;
                    picture_size(B.rc[i].img - 1, &pw, &ph);
                    wd += pw;
                } else {
                    wd += rc_w(&B.rc[i], MD_FONT_8x16, r ? 0 : MF_BOLD);
                }
            }
            wd += 2 * CELL_PAD;
            if (wd > nat[c])
                nat[c] = wd;
        }
        p = nl + 1;
    }
    // Narrowed to fit: the widest column first.
    int sum = 0;
    for (int c = 0; c < nc; ++c) {
        if (nat[c] < 3 * 8 + 2 * CELL_PAD)
            nat[c] = 3 * 8 + 2 * CELL_PAD;
        sum += nat[c];
    }
    while (sum > avail) {
        int wi = 0;
        for (int c = 1; c < nc; ++c)
            if (nat[c] > nat[wi])
                wi = c;
        if (nat[wi] <= 4 * 8 + 2 * CELL_PAD)
            break;
        nat[wi] -= 8, sum -= 8;
    }
    gap(0);
    int32_t top = B.y;
    rect(x0, B.y, sum + 1, 1, MD_C_RULE);
    B.y += 1;
    for (int r = 0; r < rows && !B.oom; ++r) {
        // The row's cells, then its background and lines (sorted in front of the text later: they are above it).
        int32_t bottom = B.y;
        int cx = x0;
        for (int c = 0; c < nc; ++c) {
            int *ce = &cell[(r * nc + c) * 2];
            int32_t yb = flow(all + ce[0], ce[1], cx + CELL_PAD + 1, nat[c] - 2 * CELL_PAD, B.y + 4, MD_FONT_8x16,
                              r ? MD_C_TEXT : MD_C_BOLD, r ? 0 : MF_BOLD, B.align[c]);
            if (yb + 4 > bottom)
                bottom = yb + 4;
            cx += nat[c];
        }
        int h = (int)(bottom - B.y);
        if (r == 0 || (r & 1) == 0)
            rect(x0 + 1, B.y, sum - 1, h, r == 0 ? MD_C_HEAD_BG : MD_C_ROW_BG);
        cx = x0;
        for (int c = 0; c <= nc; ++c) {
            rect(cx, B.y, 1, h, MD_C_RULE);
            if (c < nc)
                cx += nat[c];
        }
        rect(x0, bottom, sum + 1, 1, MD_C_RULE);
        B.y = bottom + 1;
    }
    psram_free(all);
    psram_free(cell);
    decorate(top, 24);
}

// ---- Lines ---------------------------------------------------------------------------------------

/// Adds a line to the open leaf.
/// @param s it
/// @param n its length
static void leaf_add(const char *s, int n) {
    if (!grow(&B.buf, (int *)&B.capbuf, (int)(B.nbuf + n + 2), 1))
        return;
    memcpy(B.buf + B.nbuf, s, n);
    B.nbuf += n;
    B.buf[B.nbuf++] = '\n';
    B.buf[B.nbuf] = 0;
    B.nlines++;
}

/// Lays out the open leaf and closes it.
static void leaf_close(void) {
    switch (B.leaf) {
    case L_PARA: emit_text(B.buf, (int)B.nbuf, 0); break;
    case L_FENCE:
    case L_ICODE: emit_code(B.buf, (int)B.nbuf ? (int)B.nbuf - 1 : 0, true); break;
    case L_TABLE: emit_table(B.buf, (int)B.nbuf); break;
    }
    B.leaf = L_NONE;
    B.nbuf = 0;
    B.nlines = 0;
    B.blanks = 0;
}

/// Closes the blocks from depth `to` in.
/// @param to the depth left open
static void close_to(int to) {
    while (B.depth > to) {
        cont_t *c = &B.st[B.depth - 1];
        if (c->type == C_ITEM && c->pending) {  // an empty item: its bullet on a line of its own
            gap(0);
            int32_t top = B.y;
            B.y += 16;
            decorate(top, 16);
        }
        if (c->type == C_ITEM)
            B.last[B.depth - 1] = (last_t){true, c->ordered, c->marker, c->num};
        for (int i = B.depth; i <= MAX_DEPTH; ++i)
            B.last[i].on = false;
        B.depth--;
    }
}

/// Whether a line is blank.
/// @param s it
/// @return it is
static bool blank(const char *s) {
    while (*s == ' ')
        ++s;
    return !*s;
}

/// A thematic break (*** --- ___).
/// @param s the line (indent skipped)
/// @return it is one
static bool is_rule(const char *s) {
    char c = *s;
    if (c != '*' && c != '-' && c != '_')
        return false;
    int k = 0;
    for (; *s; ++s)
        if (*s == c)
            ++k;
        else if (*s != ' ')
            return false;
    return k >= 3;
}

/// An ATX heading's level (# to ######).
/// @param s the line (indent skipped)
/// @param text set to its text
/// @param tn its length
/// @return the level, or 0
static int atx(const char *s, const char **text, int *tn) {
    int k = 0;
    while (s[k] == '#')
        ++k;
    if (k < 1 || k > 6 || (s[k] && s[k] != ' '))
        return 0;
    const char *t = s + k;
    while (*t == ' ')
        ++t;
    int n = (int)strlen(t);
    while (n && t[n - 1] == ' ')
        --n;
    int e = n;  // closing #s, if a space is before them
    while (e && t[e - 1] == '#')
        --e;
    if (e < n && (e == 0 || t[e - 1] == ' '))
        n = e;
    while (n && t[n - 1] == ' ')
        --n;
    *text = t, *tn = n;
    return k;
}

/// A fence (``` or ~~~).
/// @param s the line (indent skipped)
/// @param n set to its length
/// @return its character, or 0
static char fence(const char *s, int *n) {
    char c = *s;
    if (c != '`' && c != '~')
        return 0;
    int k = 0;
    while (s[k] == c)
        ++k;
    if (k < 3 || (c == '`' && strchr(s + k, '`')))
        return 0;
    *n = k;
    return c;
}

/// A list item's marker.
/// @param s the line (indent skipped)
/// @param ordered set to whether numbered
/// @param num set to its number
/// @param marker set to its marker character
/// @return the marker's length, or 0
static int list_marker(const char *s, bool *ordered, int *num, char *marker) {
    if ((*s == '-' || *s == '*' || *s == '+') && (s[1] == ' ' || !s[1])) {
        *ordered = false, *marker = *s, *num = 0;
        return 1;
    }
    int k = 0, v = 0;
    while (isdigit((unsigned char)s[k]) && k < 9)
        v = v * 10 + s[k++] - '0';
    if (k && (s[k] == '.' || s[k] == ')') && (s[k + 1] == ' ' || !s[k + 1])) {
        *ordered = true, *marker = s[k], *num = v;
        return k + 1;
    }
    return 0;
}

/// Whether a line starts a block that ends a paragraph (rather than continuing it).
/// @param s the line
/// @return it does
static bool interrupts(const char *s) {
    int ind = 0;
    while (s[ind] == ' ')
        ++ind;
    if (ind >= 4)
        return false;
    s += ind;
    const char *t;
    int n;
    bool o;
    int num;
    char m;
    int ml = list_marker(s, &o, &num, &m);
    return atx(s, &t, &n) || fence(s, &n) || is_rule(s) || *s == '>' || !strncmp(s, "<!--", 4) ||
           (ml && !blank(s + ml) && (!o || num == 1));
}

/// One line of the text (tabs already made spaces).
/// @param s it
static void line(char *s) {
    int n = (int)strlen(s), p = 0, matched = 0;
    if (B.leaf == L_COMMENT) {
        if (strstr(s, "-->"))
            B.leaf = L_NONE;
        return;
    }
    // The blocks it continues.
    for (; matched < B.depth; ++matched) {
        cont_t *c = &B.st[matched];
        if (c->type == C_QUOTE) {
            int q = p;
            while (q < p + 3 && s[q] == ' ')
                ++q;
            if (s[q] != '>')
                break;
            p = q + 1;
            if (s[p] == ' ')
                ++p;
        } else if (!blank(s + p)) {
            int q = p;
            while (s[q] == ' ' && q < c->col)
                ++q;
            if (q < c->col)
                break;
            p = q;
        }
    }
    bool is_blank = blank(s + p);
    if (matched < B.depth) {
        // A paragraph goes on (lazily) without the markers of the blocks round it.
        bool o;
        int num;
        char m;
        if (B.leaf == L_PARA && !is_blank && !interrupts(s + p) && !list_marker(s + p, &o, &num, &m)) {
            leaf_add(s + p, n - p);
            return;
        }
        leaf_close();
        close_to(matched);
    }
    if (B.leaf == L_FENCE) {
        int q = p, k = 0;
        while (s[q] == ' ' && q < p + 3)
            ++q;
        if (fence(s + q, &k) == B.fence_ch && k >= B.fence_n && blank(s + q + k)) {
            leaf_close();
            return;
        }
        for (int i = 0; i < B.fence_in && s[p] == ' '; ++i)
            ++p;
        leaf_add(s + p, n - p);
        return;
    }
    // New blocks.
    for (;;) {
        int q = p;
        while (s[q] == ' ' && q < p + 4)
            ++q;
        if (q - p >= 4 || (B.leaf == L_ICODE && blank(s + q)))
            break;
        if (s[q] == '>') {
            leaf_close();
            if (B.depth >= MAX_DEPTH)
                break;
            B.last[B.depth].on = false;
            cont_t *c = &B.st[B.depth];
            memset(c, 0, sizeof *c);
            c->type = C_QUOTE;
            c->x = left() + QUOTE_IN;
            if (c->x > B.width * 2 / 3)
                c->x = left();
            c->first_y = -1;
            c->task = -1;
            B.depth++;
            p = q + 1;
            if (s[p] == ' ')
                ++p;
            continue;
        }
        bool ordered;
        int num, ml;
        char marker;
        if (is_rule(s + q) || !(ml = list_marker(s + q, &ordered, &num, &marker)))
            break;
        bool empty = blank(s + q + ml);
        if (B.leaf == L_PARA && (empty || (ordered && num != 1)))
            break;
        if (B.depth >= MAX_DEPTH)
            break;
        leaf_close();
        int m = q + ml, k = 0;
        while (s[m + k] == ' ' && k < 5)
            ++k;
        int col = empty || k >= 5 ? m + 1 : m + k;
        last_t *l = &B.last[B.depth];
        cont_t *c = &B.st[B.depth];
        memset(c, 0, sizeof *c);
        c->type = C_ITEM;
        c->ordered = ordered;
        c->marker = marker;
        c->col = col;
        c->pending = true;
        c->task = -1;
        c->first_y = -1;
        bool same = l->on && l->ordered == ordered && l->marker == marker;
        c->num = ordered ? (same ? l->num + 1 : num) : 0;
        c->level = 0;
        for (int i = 0; i < B.depth; ++i)
            c->level += B.st[i].type == C_ITEM && !B.st[i].ordered;
        c->first = !same;
        int digits = 1;
        for (int v = c->num; v >= 10; v /= 10)
            ++digits;
        int in = ordered ? (digits + 1) * 8 + 8 : BULLET_IN;
        c->x = left() + (in > NUMBER_IN ? in : NUMBER_IN);
        if (c->x > B.width * 2 / 3)  // deep inside others: no further in
            c->x = left();
        B.depth++;
        p = col > n ? n : col;
        if (s[p] == '[' && (s[p + 1] == ' ' || s[p + 1] == 'x' || s[p + 1] == 'X') && s[p + 2] == ']' &&
            (s[p + 3] == ' ' || !s[p + 3])) {
            c->task = s[p + 1] != ' ';
            p += s[p + 3] ? 4 : 3;
        }
    }
    // The leaf.
    const char *r = s + p;
    int ind = 0;
    while (r[ind] == ' ')
        ++ind;
    if (blank(r)) {
        if (B.leaf == L_PARA || B.leaf == L_TABLE)
            leaf_close();
        else if (B.leaf == L_ICODE)
            B.blanks++;
        return;
    }
    if (B.leaf == L_ICODE) {
        if (ind >= 4) {
            for (; B.blanks; --B.blanks)
                leaf_add("", 0);
            leaf_add(r + 4, (int)strlen(r + 4));
            return;
        }
        leaf_close();
    }
    if (B.leaf == L_TABLE) {
        if (strchr(r, '|') && !interrupts(r)) {
            leaf_add(r, (int)strlen(r));
            return;
        }
        leaf_close();
    }
    if (B.leaf == L_PARA) {
        const char *t = r + ind;
        if (ind < 4 && (*t == '=' || *t == '-')) {  // a setext heading's underline
            int k = 0;
            while (t[k] == *t)
                ++k;
            if (blank(t + k)) {
                int level = *t == '=' ? 1 : 2;
                const char *body = B.buf;
                int bn = (int)B.nbuf;
                B.leaf = L_NONE;
                emit_text(body, bn, level);
                B.nbuf = 0, B.nlines = 0;
                return;
            }
        }
        uint8_t al[MAX_COLS];
        int dc;
        if (B.nlines == 1 && (dc = delim_row(r, al)) > 0) {
            int a[MAX_COLS], b[MAX_COLS];
            if (cells(B.buf, (int)B.nbuf, a, b, MAX_COLS) == dc) {
                B.leaf = L_TABLE;
                B.ncols = dc;
                memcpy(B.align, al, sizeof al);
                return;
            }
        }
        if (!interrupts(r)) {
            leaf_add(t, (int)strlen(t));
            return;
        }
        leaf_close();
    }
    if (ind >= 4) {
        B.leaf = L_ICODE;
        B.last[B.depth].on = false;
        leaf_add(r + 4, (int)strlen(r + 4));
        return;
    }
    B.last[B.depth].on = false;
    r += ind;
    const char *t;
    int tn, k;
    int level = atx(r, &t, &tn);
    char fc;
    if (level) {
        emit_text(t, tn, level);
    } else if ((fc = fence(r, &k))) {
        B.leaf = L_FENCE;
        B.fence_ch = fc;
        B.fence_n = k;
        B.fence_in = ind;
    } else if (is_rule(r)) {
        emit_rule();
    } else if (!strncmp(r, "<!--", 4)) {
        if (!strstr(r + 4, "-->"))
            B.leaf = L_COMMENT;
    } else {
        B.leaf = L_PARA;
        leaf_add(r, (int)strlen(r));
    }
}

/// Splits the text into lines (tabs made spaces, to stops of 4) and hands each to fn.
/// @param src the text
/// @param len its length
/// @param fn called with each line
static void each_line(const char *src, uint32_t len, void (*fn)(char *)) {
    char *l = B.l;
    const char *s = src, *end = src + len;
    if (len >= 3 && !memcmp(s, "\xef\xbb\xbf", 3))  // a byte order mark
        s += 3;
    while (s < end && !B.oom) {
        int k = 0, col = 0;
        while (s < end && *s != '\n') {
            char c = *s++;
            if (c == '\r')
                continue;
            if (c == '\t') {
                do
                    if (k < LINE_MAX)
                        l[k++] = ' ';
                while (++col % 4);
                continue;
            }
            if (k < LINE_MAX)
                l[k++] = c ? c : ' ';
            if (((unsigned char)c & 0xc0) != 0x80)
                ++col;
        }
        l[k] = 0;
        if (s < end)
            ++s;
        fn(l);
    }
}

/// The first pass: a line that defines a reference link is kept.
/// @param s the line
static void ref_line(char *s) {
    while (*s == ' ' || *s == '>')
        ++s;
    const char *l, *u;
    int ln, un;
    if (!ref_def(s, &l, &ln, &u, &un) || !grow(&B.refs, &B.caprefs, B.nrefs + 1, sizeof *B.refs))
        return;
    char tmp[512];
    snprintf(tmp, sizeof tmp, "%.*s", un, u);
    if (tmp[0] == '<') {  // <url>
        memmove(tmp, tmp + 1, strlen(tmp));
        char *gt = strchr(tmp, '>');
        if (gt)
            *gt = 0;
    }
    B.refs[B.nrefs].label = pool_add(l, ln);
    B.refs[B.nrefs].url = pool_add(tmp, (int)strlen(tmp));
    B.nrefs++;
}

/// The source view: each line laid out as it is.
/// @param s the line
static void source_line(char *s) {
    B.started = false;  // (no gaps)
    emit_code(s, (int)strlen(s), false);
}

/// Sorts items by y, keeping their order otherwise (an insertion sort: they are nearly in order).
/// @param d the document
static void sort_items(md_doc_t *d) {
    for (int i = 1; i < d->nitems; ++i) {
        if (d->items[i].y >= d->items[i - 1].y)
            continue;
        md_item_t t = d->items[i];
        int j = i - 1;
        while (j >= 0 && d->items[j].y > t.y) {
            d->items[j + 1] = d->items[j];
            --j;
        }
        d->items[j + 1] = t;
    }
}

md_doc_t *md_layout(const char *src, uint32_t len, int width, bool source, md_size_fn size_of, void *arg) {
    md_doc_t *d = psram_malloc(sizeof *d);
    Bp = psram_malloc(sizeof *Bp);
    if (!d || !Bp) {
        psram_free(d);
        psram_free(Bp);
        Bp = NULL;
        return NULL;
    }
    memset(d, 0, sizeof *d);
    memset(&B, 0, sizeof B);
    B.d = d;
    B.width = width;
    B.size_of = size_of;
    B.arg = arg;
    d->width = width;
    pool_add("", 0);  // offset 0 is ""
    B.y = 4;
    if (source) {
        each_line(src, len, source_line);
    } else {
        each_line(src, len, ref_line);
        each_line(src, len, line);
        leaf_close();
        close_to(0);
    }
    sort_items(d);
    d->height = B.y + 16;
    d->short_of_memory = B.oom;
    psram_free(B.rc);
    psram_free(B.refs);
    psram_free(B.ln);
    psram_free(B.buf);
    psram_free(Bp);
    Bp = NULL;
    return d;
}

void md_free(md_doc_t *d) {
    if (!d)
        return;
    psram_free(d->items);
    psram_free(d->pool);
    psram_free(d->links);
    psram_free(d->imgs);
    psram_free(d->heads);
    psram_free(d);
}

int md_first_item(const md_doc_t *d, int32_t y) {
    int lo = 0, hi = d->nitems;
    y -= MD_ITEM_MAX_H;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (d->items[mid].y < y)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}
