/**
 * @file www_html.c
 * @brief The web browser's page engine: reads HTML in one pass and lays it out
 *        as lines of code page 437 text, the way lynx does.
 *
 * Blocks (paragraphs, headings, lists, quotes, tables' rows) start new lines;
 * words wrap at WWW_COLS. Links and form fields keep a record of where their
 * characters are, so the browser can colour, choose and follow them. Scripts,
 * styles and pictures are left out (a picture shows its alt text).
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */

// URLs are cut to fit on purpose.
#pragma GCC diagnostic ignored "-Wformat-truncation"
#include "www_html.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "psram_heap.h"

/// No link: the cell is plain text.
#define NO_LINK 0xffff
/// Block elements nested deeper than this don't indent any further.
#define MAX_DEPTH 24
/// Attributes kept per tag.
#define MAX_ATTRS 24
/// Room for a tag's attribute names and values.
#define ATTR_BUF 3072
/// The deepest indent, so there's always room for text.
#define MAX_INDENT 36

/// An indenting block that is open: a list, a quote, a <dd>.
typedef struct {
    char tag[16];   ///< its tag name
    int indent;     ///< the indent before it
    bool ordered;   ///< an <ol>
    int count;      ///< the <ol>'s next number
} block_t;

/// The layout's state while a page is read (PSRAM).
typedef struct {
    www_doc_t *d;            ///< the page being made
    const uint8_t *p;        ///< the next byte
    const uint8_t *end;      ///< the end of the bytes
    bool latin1;             ///< ISO-8859-1 / Windows-1252 rather than UTF-8
    bool pictures;           ///< leave room for pictures in the page
    www_size_fn size_of;     ///< the sizes of pictures already loaded, or NULL
    www_toggled_fn toggled;  ///< menus switched from their default, or NULL (all open)
    int ncoll;               ///< collapsible blocks (<nav>, <details>) so far
    www_cell_t line[WWW_COLS + 1];  ///< the line being filled
    uint16_t lnk[WWW_COLS + 1];     ///< each cell's link
    bool brk[WWW_COLS + 1];         ///< a line may break at this cell (a space)
    int len;                 ///< cells in the line
    int start;               ///< where its text starts (after the indent)
    int indent;              ///< the indent for new lines
    char bullet[8];          ///< the next line's list bullet, if any
    bool space;              ///< a space is due before the next word
    int blank;               ///< empty lines at the end of the page
    int bold;                ///< <b> depth
    int ital;                ///< <i> depth
    int code;                ///< <code> depth
    int quote;               ///< <blockquote> depth
    int pre;                 ///< <pre> depth
    bool pre_start;          ///< just after <pre>: a newline here is dropped
    bool dt;                 ///< in a <dt>
    int head;                ///< in a heading: its level, or 0
    int link;                ///< the open <a> or <button>, or -1
    int link_cells;          ///< its cells so far
    int form;                ///< the open form, or -1
    int select;              ///< the open <select>, or -1
    bool in_opt;             ///< in an <option>
    char opt[80];            ///< its label so far
    int optlen;              ///< its length
    bool opt_space;          ///< a space is due in the label
    bool opt_sel;            ///< it is selected
    char optval[256];        ///< its value (UTF-8)
    bool has_optval;         ///< it had a value attribute
    int cells_in_row;        ///< table cells so far in this row
    int cell_start;          ///< where the table cell's text starts on the line, or -1
    block_t blocks[MAX_DEPTH];  ///< the indenting blocks open
    int depth;               ///< how many
    char tag[16];            ///< the tag just read, lower case
    bool close;              ///< it was an end tag
    int nattr;               ///< its attributes
    char *aname[MAX_ATTRS];  ///< their names, lower case
    char *aval[MAX_ATTRS];   ///< their values: entities decoded, UTF-8
    char abuf[ATTR_BUF];     ///< where those are
    char href[WWW_URL_MAX];  ///< room for a frame's or refresh's address
    char label[WWW_URL_MAX + 32];  ///< and what is shown for it
} bld_t;

// ---- Characters -----------------------------------------------------------------------------------

/// What CP437 bytes 0x80-0xFF are in Unicode.
static const uint16_t cp437_high[128] = {
    0x00c7, 0x00fc, 0x00e9, 0x00e2, 0x00e4, 0x00e0, 0x00e5, 0x00e7, 0x00ea, 0x00eb, 0x00e8,
    0x00ef, 0x00ee, 0x00ec, 0x00c4, 0x00c5, 0x00c9, 0x00e6, 0x00c6, 0x00f4, 0x00f6, 0x00f2,
    0x00fb, 0x00f9, 0x00ff, 0x00d6, 0x00dc, 0x00a2, 0x00a3, 0x00a5, 0x20a7, 0x0192, 0x00e1,
    0x00ed, 0x00f3, 0x00fa, 0x00f1, 0x00d1, 0x00aa, 0x00ba, 0x00bf, 0x2310, 0x00ac, 0x00bd,
    0x00bc, 0x00a1, 0x00ab, 0x00bb, 0x2591, 0x2592, 0x2593, 0x2502, 0x2524, 0x2561, 0x2562,
    0x2556, 0x2555, 0x2563, 0x2551, 0x2557, 0x255d, 0x255c, 0x255b, 0x2510, 0x2514, 0x2534,
    0x252c, 0x251c, 0x2500, 0x253c, 0x255e, 0x255f, 0x255a, 0x2554, 0x2569, 0x2566, 0x2560,
    0x2550, 0x256c, 0x2567, 0x2568, 0x2564, 0x2565, 0x2559, 0x2558, 0x2552, 0x2553, 0x256b,
    0x256a, 0x2518, 0x250c, 0x2588, 0x2584, 0x258c, 0x2590, 0x2580, 0x03b1, 0x00df, 0x0393,
    0x03c0, 0x03a3, 0x03c3, 0x00b5, 0x03c4, 0x03a6, 0x0398, 0x03a9, 0x03b4, 0x221e, 0x03c6,
    0x03b5, 0x2229, 0x2261, 0x00b1, 0x2265, 0x2264, 0x2320, 0x2321, 0x00f7, 0x2248, 0x00b0,
    0x2219, 0x00b7, 0x221a, 0x207f, 0x00b2, 0x25a0, 0x00a0,
};

/// Latin-1 letters 0xC0-0xFF without their accents, for those CP437 lacks.
static const char latin1_fold[] = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYPsaaaaaaaceeeeiiiidnooooo/ouuuuyPy";

uint8_t www_cp437(uint32_t u) {
    if (u < 0x80)
        return u < 0x20 ? ' ' : (uint8_t)u;
    if (u == 0xa0)
        return ' ';
    for (int i = 0; i < 128; ++i)
        if (cp437_high[i] == u)
            return 0x80 + i;
    if (u >= 0xc0 && u <= 0xff)
        return (uint8_t)latin1_fold[u - 0xc0];
    switch (u) {
    case 0xad: case 0x200b: case 0x200c: case 0x200d: case 0x200e: case 0x200f: case 0x2060:
    case 0xfeff: case 0xfe0f:
        return 0;  // invisible
    case 0x2018: case 0x2019: case 0x201a: case 0x201b: case 0x2032: case 0xb4: return '\'';
    case 0x201c: case 0x201d: case 0x201e: case 0x2033: return '"';
    case 0x2010: case 0x2011: case 0x2012: case 0x2013: case 0x2014: case 0x2015:
    case 0x2212: return '-';
    case 0x2022: case 0x25cf: case 0x2023: case 0x2043: return 0x07;
    case 0x2039: return '<';
    case 0x203a: return '>';
    case 0x2190: return 0x1b;
    case 0x2191: return 0x18;
    case 0x2192: return 0x1a;
    case 0x2193: return 0x19;
    case 0x2194: return 0x1d;
    case 0x2195: return 0x12;
    case 0x25b2: case 0x25b4: return 0x1e;
    case 0x25bc: case 0x25be: return 0x1f;
    case 0x25b6: case 0x25b8: return 0x10;
    case 0x25c0: case 0x25c2: return 0x11;
    case 0x2660: return 0x06;
    case 0x2663: return 0x05;
    case 0x2665: case 0x2764: return 0x03;
    case 0x2666: return 0x04;
    case 0x263a: return 0x01;
    case 0x266a: return 0x0d;
    case 0x266b: return 0x0e;
    case 0x2713: case 0x2714: return 0xfb;
    case 0x2717: case 0x2718: case 0xd7: return 'x';
    case 0x2260: return '#';
    case 0x2026: return '.';
    case 0x00a6: return 0xb3;
    case 0x00a8: case 0x00af: case 0x00b8: return 0;
    case 0x00b9: return '1';
    case 0x00b3: return '3';
    case 0x00be: return '3';
    case 0x00a4: return '$';
    case 0x00a9: return 'c';
    case 0x00ae: return 'R';
    case 0x00a7: return 0x15;
    case 0x00b6: return 0x14;
    case 0x0152: return 'O';
    case 0x0153: return 'o';
    case 0x0160: return 'S';
    case 0x0161: return 's';
    case 0x017d: return 'Z';
    case 0x017e: return 'z';
    case 0x0178: return 'Y';
    case 0x02c6: return '^';
    case 0x02dc: return '~';
    }
    if (u >= 0x2000 && u <= 0x200a)
        return ' ';  // the typographic spaces
    if ((u >= 0x300 && u <= 0x36f) || u >= 0x1f000)
        return 0;  // combining accents, emoji
    if (u >= 0x2500 && u <= 0x257f)  // other lines: heavy, dashed
        return (u & 2) ? 0xb3 : 0xc4;
    if (u >= 0x2580 && u <= 0x259f)
        return 0xdb;
    return '?';
}

/// Longer stand-ins for a few code points, or NULL.
/// @param u the code point
/// @return the text
static const char *expand(uint32_t u) {
    switch (u) {
    case 0x2026: return "...";
    case 0x2122: return "(TM)";
    case 0x00a9: return "(c)";
    case 0x00ae: return "(R)";
    case 0x20ac: return "EUR";
    case 0x00be: return "3/4";
    case 0x2153: return "1/3";
    case 0x2154: return "2/3";
    }
    return NULL;
}

/// Windows-1252's 0x80-0x9F, where ISO-8859-1 has control codes.
static const uint16_t cp1252[32] = {
    0x20ac, 0x81, 0x201a, 0x0192, 0x201e, 0x2026, 0x2020, 0x2021, 0x02c6, 0x2030, 0x0160,
    0x2039, 0x0152, 0x8d, 0x017d, 0x8f, 0x90, 0x2018, 0x2019, 0x201c, 0x201d, 0x2022,
    0x2013, 0x2014, 0x02dc, 0x2122, 0x0161, 0x203a, 0x0153, 0x9d, 0x017e, 0x0178,
};

/// The next character of the page as a code point.
/// @param b the state
/// @return the code point
static uint32_t next_cp(bld_t *b) {
    uint8_t c = *b->p++;
    if (c < 0x80)
        return c;
    if (b->latin1)
        return c < 0xa0 ? cp1252[c - 0x80] : c;
    int n = c >= 0xf8 ? -1 : c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : c >= 0xc0 ? 1 : -1;
    if (n < 0 || b->p + n > b->end)
        return c;  // not UTF-8 after all: take it as Latin-1
    uint32_t u = c & (0x3f >> n);
    for (int i = 0; i < n; ++i) {
        if ((b->p[i] & 0xc0) != 0x80)
            return c;
        u = u << 6 | (b->p[i] & 0x3f);
    }
    b->p += n;
    return u;
}

/// Appends a code point as UTF-8.
/// @param u the code point
/// @param out where
/// @param n its length so far (moved on)
/// @param size room
static void put_utf8(uint32_t u, char *out, int *n, int size) {
    char t[4];
    int k = 0;
    if (u < 0x80) {
        t[k++] = (char)u;
    } else if (u < 0x800) {
        t[k++] = (char)(0xc0 | u >> 6);
        t[k++] = (char)(0x80 | (u & 0x3f));
    } else if (u < 0x10000) {
        t[k++] = (char)(0xe0 | u >> 12);
        t[k++] = (char)(0x80 | (u >> 6 & 0x3f));
        t[k++] = (char)(0x80 | (u & 0x3f));
    } else {
        t[k++] = (char)(0xf0 | u >> 18);
        t[k++] = (char)(0x80 | (u >> 12 & 0x3f));
        t[k++] = (char)(0x80 | (u >> 6 & 0x3f));
        t[k++] = (char)(0x80 | (u & 0x3f));
    }
    if (*n + k < size) {
        memcpy(out + *n, t, k);
        *n += k;
    }
}

uint32_t www_unicode(uint8_t c) {
    return c < 0x80 ? c : cp437_high[c - 0x80];
}

/// Code points of the named entities (besides the Latin-1 ones).
static const struct {
    const char *name;  ///< without & and ;
    uint16_t u;        ///< the code point
} entities[] = {
    {"amp", '&'}, {"lt", '<'}, {"gt", '>'}, {"quot", '"'}, {"apos", '\''},
    {"ndash", 0x2013}, {"mdash", 0x2014}, {"lsquo", 0x2018}, {"rsquo", 0x2019},
    {"sbquo", 0x201a}, {"ldquo", 0x201c}, {"rdquo", 0x201d}, {"bdquo", 0x201e},
    {"hellip", 0x2026}, {"bull", 0x2022}, {"trade", 0x2122}, {"euro", 0x20ac},
    {"larr", 0x2190}, {"uarr", 0x2191}, {"rarr", 0x2192}, {"darr", 0x2193}, {"harr", 0x2194},
    {"lsaquo", 0x2039}, {"rsaquo", 0x203a}, {"dagger", 0x2020}, {"Dagger", 0x2021},
    {"permil", 0x2030}, {"prime", 0x2032}, {"Prime", 0x2033}, {"minus", 0x2212},
    {"le", 0x2264}, {"ge", 0x2265}, {"ne", 0x2260}, {"infin", 0x221e}, {"asymp", 0x2248},
    {"equiv", 0x2261}, {"radic", 0x221a}, {"sum", 0x03a3}, {"pi", 0x03c0}, {"alpha", 0x03b1},
    {"beta", 0x00df}, {"mu", 0x00b5}, {"Omega", 0x03a9}, {"sigma", 0x03c3}, {"delta", 0x03b4},
    {"thinsp", 0x2009}, {"ensp", 0x2002}, {"emsp", 0x2003}, {"zwnj", 0x200c}, {"zwj", 0x200d},
    {"lrm", 0x200e}, {"rlm", 0x200f}, {"hearts", 0x2665}, {"spades", 0x2660},
    {"clubs", 0x2663}, {"diams", 0x2666}, {"check", 0x2713}, {"star", '*'}, {"times", 0xd7},
    {"OElig", 0x152}, {"oelig", 0x153}, {"Scaron", 0x160}, {"scaron", 0x161}, {"circ", 0x2c6},
    {"tilde", 0x2dc}, {"nbsp", 0xa0}, {"NewLine", '\n'}, {"Tab", '\t'},
};

/// The Latin-1 entities, 160 to 255 in order.
static const char *const latin1_names[96] = {
    "nbsp", "iexcl", "cent", "pound", "curren", "yen", "brvbar", "sect", "uml", "copy",
    "ordf", "laquo", "not", "shy", "reg", "macr", "deg", "plusmn", "sup2", "sup3", "acute",
    "micro", "para", "middot", "cedil", "sup1", "ordm", "raquo", "frac14", "frac12", "frac34",
    "iquest", "Agrave", "Aacute", "Acirc", "Atilde", "Auml", "Aring", "AElig", "Ccedil",
    "Egrave", "Eacute", "Ecirc", "Euml", "Igrave", "Iacute", "Icirc", "Iuml", "ETH", "Ntilde",
    "Ograve", "Oacute", "Ocirc", "Otilde", "Ouml", "times", "Oslash", "Ugrave", "Uacute",
    "Ucirc", "Uuml", "Yacute", "THORN", "szlig", "agrave", "aacute", "acirc", "atilde",
    "auml", "aring", "aelig", "ccedil", "egrave", "eacute", "ecirc", "euml", "igrave",
    "iacute", "icirc", "iuml", "eth", "ntilde", "ograve", "oacute", "ocirc", "otilde",
    "ouml", "divide", "oslash", "ugrave", "uacute", "ucirc", "uuml", "yacute", "thorn",
    "yuml",
};

/// Reads an entity at `*p` (which is at '&'); moves past it.
/// @param p the text (moved on)
/// @param end its end
/// @return the code point ('&' if it isn't one)
static uint32_t entity(const uint8_t **p, const uint8_t *end) {
    const uint8_t *s = *p + 1;
    if (s < end && *s == '#') {
        ++s;
        bool hex = s < end && (*s == 'x' || *s == 'X');
        s += hex;
        uint32_t u = 0;
        const uint8_t *digits = s;
        while (s < end && (hex ? isxdigit(*s) : isdigit(*s)) && s - digits < 8) {
            u = u * (hex ? 16 : 10) + (isdigit(*s) ? *s - '0' : (*s | 0x20) - 'a' + 10);
            ++s;
        }
        if (s == digits)
            goto not_one;
        if (s < end && *s == ';')
            ++s;
        *p = s;
        if (u >= 0x80 && u < 0xa0)
            u = cp1252[u - 0x80];  // as browsers do
        return u ? u : '?';
    }
    char name[12];
    int n = 0;
    while (s + n < end && isalnum(s[n]) && n < (int)sizeof name - 1) {
        name[n] = (char)s[n];
        ++n;
    }
    name[n] = 0;
    if (!n)
        goto not_one;
    uint32_t u = 0;
    for (unsigned i = 0; i < sizeof entities / sizeof *entities && !u; ++i)
        if (!strcmp(entities[i].name, name))
            u = entities[i].u;
    for (int i = 0; i < 96 && !u; ++i)
        if (!strcmp(latin1_names[i], name))
            u = 160 + i;
    if (!u)
        goto not_one;
    s += n;
    if (s < end && *s == ';')
        ++s;
    *p = s;
    return u;
not_one:
    ++*p;
    return '&';
}

// ---- The page's arrays ------------------------------------------------------------------------------

/// Makes room for `need` elements of an array, growing it by half again.
/// @param d the page (oom is set on failure)
/// @param arr the array
/// @param cap its room (updated)
/// @param size an element's size
/// @param need the elements wanted
/// @return there is room
static bool grow(www_doc_t *d, void **arr, uint32_t *cap, size_t size, uint32_t need) {
    if (need <= *cap)
        return true;
    if (d->oom)
        return false;
    uint32_t n = *cap ? *cap + *cap / 2 : 64;
    if (n < need)
        n = need;
    void *p = psram_realloc(*arr, (size_t)n * size);
    if (!p) {
        d->oom = true;
        return false;
    }
    *arr = p;
    *cap = n;
    return true;
}

/// GROW(d, field, n): room for n elements of d->field (d->cap_field).
#define GROW(d, f, n) grow((d), (void **)&(d)->f, (uint32_t *)&(d)->cap_##f, sizeof *(d)->f, (n))

/// Adds a string to the pool.
/// @param d the page
/// @param s the bytes
/// @param n how many
/// @return its offset (0, the empty string, if there was no room)
static uint32_t pool_add(www_doc_t *d, const char *s, size_t n) {
    if (!GROW(d, pool, d->npool + (uint32_t)n + 1))
        return 0;
    uint32_t at = d->npool;
    memcpy(d->pool + at, s, n);
    d->pool[at + n] = 0;
    d->npool += (uint32_t)n + 1;
    return at;
}

/// A new link or field.
/// @param b the state
/// @param type WL_
/// @return its index, or -1
static int new_link(bld_t *b, int type) {
    www_doc_t *d = b->d;
    if (d->nlinks >= NO_LINK - 1 || !GROW(d, links, d->nlinks + 1))
        return -1;
    www_link_t *l = &d->links[d->nlinks];
    memset(l, 0, sizeof *l);
    l->type = (uint8_t)type;
    l->form = (int16_t)b->form;
    l->seg = -1;
    return d->nlinks++;
}

// ---- Lines ----------------------------------------------------------------------------------------

/// Ends the line being filled (an empty one if nothing is in it).
/// @param b the state
static void end_line(bld_t *b) {
    www_doc_t *d = b->d;
    int n = b->len;
    while (n > 0 && b->line[n - 1].ch == ' ' && (b->lnk[n - 1] == NO_LINK || b->brk[n - 1]))
        --n;
    if (n <= b->start)
        n = 0;  // only an indent (or a bullet with nothing after it): empty
    if (!GROW(d, lines, d->nlines + 1) || !GROW(d, cells, d->ncells + n)) {
        b->len = 0;
        return;
    }
    d->lines[d->nlines] = (www_line_t){d->ncells, (uint16_t)n};
    if (n)
        memcpy(d->cells + d->ncells, b->line, n * sizeof *b->line);
    d->ncells += n;
    for (int i = 0; i < n;) {
        if (b->lnk[i] == NO_LINK) {
            ++i;
            continue;
        }
        int j = i;
        while (j < n && b->lnk[j] == b->lnk[i])
            ++j;
        if (!GROW(d, segs, d->nsegs + 1))
            break;
        d->segs[d->nsegs] = (www_seg_t){d->nlines, (uint8_t)i, (uint8_t)(j - i), b->lnk[i]};
        if (d->links[b->lnk[i]].seg < 0)
            d->links[b->lnk[i]].seg = d->nsegs;
        ++d->nsegs;
        i = j;
    }
    ++d->nlines;
    b->blank = n ? 0 : b->blank + 1;
    b->len = 0;
    b->start = 0;
    if (b->cell_start > 0)
        b->cell_start = 0;
}

/// Starts a line: the indent, and a list bullet if one is due.
/// @param b the state
static void open_line(bld_t *b) {
    int ind = b->indent > MAX_INDENT ? MAX_INDENT : b->indent;
    int blen = (int)strlen(b->bullet);
    int pad = blen ? ind - blen - 1 : ind;
    if (pad < 0)
        pad = 0;
    b->len = 0;
    for (int i = 0; i < pad; ++i) {
        b->line[b->len] = (www_cell_t){' ', WS_TEXT};
        b->lnk[b->len] = NO_LINK;
        b->brk[b->len++] = false;
    }
    for (int i = 0; i < blen; ++i) {
        b->line[b->len] = (www_cell_t){(uint8_t)b->bullet[i], WS_RULE};
        b->lnk[b->len] = NO_LINK;
        b->brk[b->len++] = false;
    }
    if (blen) {
        b->line[b->len] = (www_cell_t){' ', WS_TEXT};
        b->lnk[b->len] = NO_LINK;
        b->brk[b->len++] = false;
    }
    b->bullet[0] = 0;
    b->start = b->len;
}

/// The line is full: it breaks at its last space, or (if it has none) here.
/// @param b the state
static void wrap(bld_t *b) {
    int k = b->len - 1;
    while (k > b->start && !b->brk[k])
        --k;
    if (k <= b->start) {
        end_line(b);
        open_line(b);
        return;
    }
    int tail = b->len - k - 1;
    www_cell_t cells[WWW_COLS];
    uint16_t lnk[WWW_COLS];
    bool brk[WWW_COLS];
    memcpy(cells, b->line + k + 1, tail * sizeof *cells);
    memcpy(lnk, b->lnk + k + 1, tail * sizeof *lnk);
    memcpy(brk, b->brk + k + 1, tail * sizeof *brk);
    b->len = k;
    end_line(b);
    open_line(b);
    if (b->len + tail > WWW_COLS)
        tail = WWW_COLS - b->len;
    memcpy(b->line + b->len, cells, tail * sizeof *cells);
    memcpy(b->lnk + b->len, lnk, tail * sizeof *lnk);
    memcpy(b->brk + b->len, brk, tail * sizeof *brk);
    b->len += tail;
}

/// Puts a character on the line, wrapping it if it's full.
/// @param b the state
/// @param ch the character
/// @param st its style
/// @param brk the line may break here (a space between words)
/// @param link its link, or -1
static void put(bld_t *b, uint8_t ch, uint8_t st, bool brk, int link) {
    if (b->len == 0)
        open_line(b);
    if (b->len >= WWW_COLS) {
        if (brk) {  // a space at the end of a line: the break
            end_line(b);
            return;
        }
        wrap(b);
    }
    b->line[b->len] = (www_cell_t){ch, st};
    b->lnk[b->len] = link < 0 ? NO_LINK : (uint16_t)link;
    b->brk[b->len++] = brk;
    if (link >= 0)
        ++b->link_cells;
    b->blank = 0;
}

/// Starts a new line unless the line is empty.
/// @param b the state
static void newline(bld_t *b) {
    if (b->cell_start >= 0 && b->len <= b->cell_start) {
        b->space = false;  // a block at the start of a table cell: same line
        return;
    }
    if (b->len > b->start)
        end_line(b);
    else
        b->len = 0;
    b->space = false;
}

/// A paragraph break: one empty line (never two, nor one at the top).
/// @param b the state
static void para(bld_t *b) {
    newline(b);
    if (b->d->nlines > 0 && b->blank == 0)
        end_line(b);
}

/// The style for text here.
/// @param b the state
/// @return WS_
static uint8_t style(const bld_t *b) {
    if (b->head == 1)
        return WS_H1;
    if (b->head)
        return WS_H2;
    if (b->code || b->pre)
        return WS_CODE;
    if (b->bold || b->dt)
        return WS_BOLD;
    if (b->ital)
        return WS_ITALIC;
    if (b->quote)
        return WS_QUOTE;
    return WS_TEXT;
}

/// Adds a character to the <option> being read.
/// @param b the state
/// @param u the code point
static void opt_char(bld_t *b, uint32_t u) {
    if (u <= ' ' || u == 0xa0) {
        b->opt_space = b->optlen > 0;
        return;
    }
    uint8_t c = www_cp437(u);
    if (!c)
        return;
    if (b->opt_space && b->optlen < (int)sizeof b->opt - 1)
        b->opt[b->optlen++] = ' ';
    b->opt_space = false;
    if (b->optlen < (int)sizeof b->opt - 1)
        b->opt[b->optlen++] = (char)c;
}

/// Text: a code point of the page.
/// @param b the state
/// @param u the code point
/// @param st the style, or -1 for the one here
static void text(bld_t *b, uint32_t u, int st) {
    if (b->select >= 0) {
        if (b->in_opt)
            opt_char(b, u);
        return;
    }
    uint8_t s = st < 0 ? style(b) : (uint8_t)st;
    if (b->pre) {
        if (u == '\r')
            return;
        if (u == '\n') {
            if (b->pre_start) {
                b->pre_start = false;
                return;
            }
            end_line(b);  // (an empty line if nothing is on it)
            return;
        }
        b->pre_start = false;
        if (u == '\t') {
            if (b->len == 0)
                open_line(b);
            do
                put(b, ' ', s, false, b->link);
            while ((b->len - b->start) % 8 && b->len < WWW_COLS);
            return;
        }
        uint8_t c = www_cp437(u);
        if (c)
            put(b, c, s, false, b->link);
        return;
    }
    if (u == ' ' || u == '\t' || u == '\n' || u == '\r' || u == '\f') {
        b->space = true;
        return;
    }
    if (u < 0x20)
        return;
    const char *more = expand(u);
    uint8_t c = more ? 1 : www_cp437(u);
    if (!c)
        return;
    if (b->space) {  // (between a link's words, the space is the link's)
        if (b->len > b->start)
            put(b, ' ', WS_TEXT, true, b->link >= 0 && b->link_cells ? b->link : -1);
        b->space = false;
    }
    if (more) {
        while (*more)
            put(b, (uint8_t)*more++, s, false, b->link);
        return;
    }
    put(b, c, s, false, b->link);
}

/// Text from an attribute (UTF-8), e.g. an image's alt text.
/// @param b the state
/// @param s the text
/// @param st the style
static void text_utf8(bld_t *b, const char *s, int st) {
    const uint8_t *save_p = b->p, *save_end = b->end;
    bool save_l1 = b->latin1;
    b->p = (const uint8_t *)s;
    b->end = b->p + strlen(s);
    b->latin1 = false;
    while (b->p < b->end)
        text(b, next_cp(b), st);
    b->p = save_p;
    b->end = save_end;
    b->latin1 = save_l1;
}

/// Converts UTF-8 to code page 437.
/// @param s the text
/// @param out the CP437 text, spaces collapsed
/// @param size room in out
static void to_cp437(const char *s, char *out, size_t size) {
    size_t n = 0;
    bool space = false;
    const uint8_t *p = (const uint8_t *)s, *end = p + strlen(s);
    bld_t tmp;  // only p, end and latin1 are used
    tmp.p = p;
    tmp.end = end;
    tmp.latin1 = false;
    while (tmp.p < tmp.end && n + 1 < size) {
        uint32_t u = next_cp(&tmp);
        if (u <= ' ' || u == 0xa0) {
            space = n > 0;
            continue;
        }
        uint8_t c = www_cp437(u);
        if (!c)
            continue;
        if (space && n + 2 < size)
            out[n++] = ' ';
        space = false;
        out[n++] = (char)c;
    }
    out[n] = 0;
}

// ---- Form fields ----------------------------------------------------------------------------------

/// A field's characters on the page.
/// @param d the page
/// @param l the field
/// @param out its characters (l->width of them)
static void render_field(const www_doc_t *d, const www_link_t *l, uint8_t *out) {
    int w = l->width;
    const char *v = l->type == WL_SELECT ? www_option(d, (int)(l - d->links), l->sel, false)
                                         : www_str(d, l->value);
    switch (l->type) {
    case WL_CHECK:
        memcpy(out, l->checked ? "[X]" : "[ ]", 3);
        return;
    case WL_RADIO:
        memcpy(out, l->checked ? "(*)" : "( )", 3);
        return;
    case WL_SELECT: {
        int n = (int)strlen(v);
        out[0] = '[';
        for (int i = 1; i < w - 2; ++i)
            out[i] = i - 1 < n ? (uint8_t)v[i - 1] : ' ';
        out[w - 2] = 0x1f;
        out[w - 1] = ']';
        return;
    }
    case WL_SUBMIT: {
        int n = (int)strlen(v);
        out[0] = '[';
        for (int i = 1; i < w - 1; ++i)
            out[i] = i - 1 < n ? (uint8_t)v[i - 1] : ' ';
        out[w - 1] = ']';
        return;
    }
    default: {
        int n = (int)strlen(v);
        for (int i = 0; i < w; ++i)
            out[i] = i < n ? (l->type == WL_PASSWORD ? '*' : (uint8_t)v[i]) : '_';
        if (n > w)
            out[w - 1] = 0xaf;  // more than fits
    }
    }
}

/// Puts a field on the page, all on one line.
/// @param b the state
/// @param link the field
static void emit_field(bld_t *b, int link) {
    www_doc_t *d = b->d;
    www_link_t *l = &d->links[link];
    int w = l->width;
    if (w > WWW_COLS - MAX_INDENT)
        w = l->width = WWW_COLS - MAX_INDENT;
    if (b->space && b->len > b->start) {
        put(b, ' ', WS_TEXT, true, -1);
    }
    b->space = false;
    if (b->len && b->len + w > WWW_COLS)
        end_line(b);
    uint8_t cells[WWW_COLS];
    render_field(d, l, cells);
    for (int i = 0; i < w; ++i)
        put(b, cells[i], WS_FIELD, false, link);
}

void www_redraw_field(www_doc_t *d, int link) {
    www_link_t *l = &d->links[link];
    if (l->seg < 0 || l->width < 3)  // (a <button> is its own text: width 0)
        return;
    uint8_t cells[WWW_COLS];
    render_field(d, l, cells);
    int k = 0;
    for (int32_t s = l->seg; s < d->nsegs && d->segs[s].link == link && k < l->width; ++s) {
        www_cell_t *c = d->cells + d->lines[d->segs[s].line].cell + d->segs[s].col;
        for (int i = 0; i < d->segs[s].len && k < l->width; ++i)
            c[i].ch = cells[k++];
    }
}

bool www_set_value(www_doc_t *d, int link, const char *value) {
    uint32_t v = pool_add(d, value, strlen(value));
    if (!v && value[0])
        return false;
    d->links[link].value = v;
    www_redraw_field(d, link);
    return true;
}

const char *www_option(const www_doc_t *d, int link, int i, bool value) {
    const www_link_t *l = &d->links[link];
    if (i >= l->nopts)
        return "";
    const char *s = www_str(d, l->opts);
    for (int k = 0; k < 2 * i + value; ++k)
        s += strlen(s) + 1;
    return s;
}

/// The value of attribute `name`, or NULL.
/// @param b the state
/// @param name the attribute
/// @return its value (UTF-8), or NULL
static const char *attr(const bld_t *b, const char *name) {
    for (int i = 0; i < b->nattr; ++i)
        if (!strcmp(b->aname[i], name))
            return b->aval[i];
    return NULL;
}

/// An attribute's value as a number.
/// @param b the state
/// @param name the attribute
/// @param def the number if it's missing
/// @return its value
static int attr_int(const bld_t *b, const char *name, int def) {
    const char *v = attr(b, name);
    return v && isdigit((unsigned char)*v) ? atoi(v) : def;
}

/// The <option> read so far goes into the <select>.
/// @param b the state
static void end_option(bld_t *b) {
    if (!b->in_opt || b->select < 0)
        return;
    b->in_opt = false;
    www_doc_t *d = b->d;
    www_link_t *l = &d->links[b->select];
    b->opt[b->optlen] = 0;
    uint32_t at = pool_add(d, b->opt, b->optlen);
    if (!b->has_optval) {  // the value is the label, sent as UTF-8
        int n = 0;
        for (int i = 0; i < b->optlen; ++i)
            put_utf8(www_unicode((uint8_t)b->opt[i]), b->optval, &n, sizeof b->optval);
        b->optval[n] = 0;
    }
    pool_add(d, b->optval, strlen(b->optval));
    if (d->oom)
        return;
    if (!l->nopts)
        l->opts = at;
    if (b->opt_sel)
        l->sel = l->nopts;
    ++l->nopts;
}

/// </select>: the field goes on the page.
/// @param b the state
static void end_select(bld_t *b) {
    if (b->select < 0)
        return;
    end_option(b);
    int link = b->select;
    b->select = -1;
    www_link_t *l = &b->d->links[link];
    int w = 0;
    for (int i = 0; i < l->nopts; ++i) {
        int n = (int)strlen(www_option(b->d, link, i, false));
        if (n > w)
            w = n;
    }
    w = w < 4 ? 4 : w > 36 ? 36 : w;
    l->width = (uint8_t)(w + 3);
    emit_field(b, link);
}

/// Makes a field from a tag's name and value attributes.
/// @param b the state
/// @param type WL_
/// @param value its value (CP437 unless utf8), or NULL
/// @param utf8 keep the value as UTF-8 (hidden fields, checkboxes)
/// @return the field, or -1
static int field(bld_t *b, int type, const char *value, bool utf8) {
    int link = new_link(b, type);
    if (link < 0)
        return -1;
    www_doc_t *d = b->d;
    const char *name = attr(b, "name");
    uint32_t n = pool_add(d, name ? name : "", name ? strlen(name) : 0);
    char cp[256];
    if (value && !utf8) {
        to_cp437(value, cp, sizeof cp);
        value = cp;
    }
    uint32_t v = pool_add(d, value ? value : "", value ? strlen(value) : 0);
    d->links[link].href = n;
    d->links[link].value = v;
    return link;
}

// ---- Tags -----------------------------------------------------------------------------------------

/// Is it this tag?
#define IS(t) (!strcmp(b->tag, (t)))

/// Opens an indenting block.
/// @param b the state
/// @param add how much more to indent
/// @param ordered an <ol>
static void push_block(bld_t *b, int add, bool ordered) {
    if (b->depth < MAX_DEPTH) {
        block_t *k = &b->blocks[b->depth++];
        snprintf(k->tag, sizeof k->tag, "%s", b->tag);
        k->indent = b->indent;
        k->ordered = ordered;
        k->count = attr_int(b, "start", 1);
    }
    b->indent += add;
}

/// Closes the block `tag` and any opened inside it (if it is open at all).
/// @param b the state
/// @param tag its tag
static void pop_block(bld_t *b, const char *tag) {
    for (int i = b->depth - 1; i >= 0; --i)
        if (!strcmp(b->blocks[i].tag, tag)) {
            b->indent = b->blocks[i].indent;
            b->depth = i;
            return;
        }
}

/// Ends the open link (<a> or <button>).
/// @param b the state
static void end_link(bld_t *b) {
    if (b->link < 0)
        return;
    www_link_t *l = &b->d->links[b->link];
    if (l->type == WL_SUBMIT) {
        if (b->link_cells <= 1)
            text_utf8(b, "Submit", WS_FIELD);
        put(b, ']', WS_FIELD, false, b->link);
    }
    b->link = -1;
}

/// Starts a link to `href` showing `label` (for frames and refreshes).
/// @param b the state
/// @param href the address
/// @param label what is shown
static void link_line(bld_t *b, const char *href, const char *label) {
    newline(b);
    int link = new_link(b, WL_HREF);
    if (link < 0)
        return;
    b->d->links[link].href = pool_add(b->d, href, strlen(href));
    b->link = link;
    b->link_cells = 0;
    text_utf8(b, label, WS_TEXT);
    b->link = -1;
    newline(b);
}

/// Skips to the end tag `name` (for <script>, <style> and the like).
/// @param b the state
/// @param name the tag's name
/// @param from set to where the skipped text starts, if wanted
/// @return where it ends (the '<' of the end tag)
static const uint8_t *skip_to_end_tag(bld_t *b, const char *name, const uint8_t **from) {
    size_t n = strlen(name);
    const uint8_t *s = b->p;
    if (from)
        *from = s;
    for (; s + n + 2 <= b->end; ++s)
        if (s[0] == '<' && s[1] == '/' && !strncasecmp((const char *)s + 2, name, n) &&
            (s + n + 2 == b->end || !isalnum(s[n + 2]))) {
            const uint8_t *gt = memchr(s, '>', b->end - s);
            b->p = gt ? gt + 1 : b->end;
            return s;
        }
    b->p = b->end;
    return b->end;
}

/// Raw text (a title or a textarea): entities decoded, spaces collapsed, CP437.
/// @param b the state
/// @param from where it starts
/// @param to where it ends
/// @param out the text
/// @param size room in out
static void raw_text(bld_t *b, const uint8_t *from, const uint8_t *to, char *out, size_t size) {
    bld_t tmp;
    tmp.p = from;
    tmp.end = to;
    tmp.latin1 = b->latin1;
    size_t n = 0;
    bool space = false;
    while (tmp.p < tmp.end && n + 1 < size) {
        uint32_t u = *tmp.p == '&' ? entity(&tmp.p, tmp.end) : next_cp(&tmp);
        if (u <= ' ' || u == 0xa0) {
            space = n > 0;
            continue;
        }
        uint8_t c = www_cp437(u);
        if (!c)
            continue;
        if (space && n + 2 < size)
            out[n++] = ' ';
        space = false;
        out[n++] = (char)c;
    }
    out[n] = 0;
}

/// Finds `what` in `s`, any case.
/// @param s the text
/// @param what what
/// @return where, or NULL
static const char *find_ci(const char *s, const char *what) {
    size_t n = strlen(what);
    for (; *s; ++s)
        if (!strncasecmp(s, what, n))
            return s;
    return NULL;
}

/// A <meta>: the character set, or a refresh to another page.
/// @param b the state
static void meta(bld_t *b) {
    const char *cs = attr(b, "charset");
    const char *equiv = attr(b, "http-equiv");
    const char *content = attr(b, "content");
    if (equiv && content && !strcasecmp(equiv, "content-type"))
        cs = find_ci(content, "charset=") ? find_ci(content, "charset=") + 8 : NULL;
    if (cs)
        b->latin1 = find_ci(cs, "8859") || find_ci(cs, "1252") || find_ci(cs, "latin");
    if (equiv && content && !strcasecmp(equiv, "refresh")) {
        const char *u = find_ci(content, "url=");
        if (u) {
            u += 4;
            snprintf(b->href, sizeof b->href, "%s", u + (*u == '\'' || *u == '"'));
            size_t n = strlen(b->href);
            if (n && (b->href[n - 1] == '\'' || b->href[n - 1] == '"'))
                b->href[n - 1] = 0;
            snprintf(b->label, sizeof b->label, "REFRESH (%d sec): %s", atoi(content), b->href);
            link_line(b, b->href, b->label);
        }
    }
}

/// An <input>.
/// @param b the state
static void input(bld_t *b) {
    const char *t = attr(b, "type");
    const char *value = attr(b, "value");
    char type[16];
    snprintf(type, sizeof type, "%s", t ? t : "text");
    for (char *c = type; *c; ++c)
        *c = (char)tolower((unsigned char)*c);
    int link;
    if (!strcmp(type, "hidden")) {
        field(b, WL_HIDDEN, value, true);
    } else if (!strcmp(type, "checkbox") || !strcmp(type, "radio")) {
        link = field(b, type[0] == 'c' ? WL_CHECK : WL_RADIO, value ? value : "on", true);
        if (link >= 0) {
            b->d->links[link].checked = attr(b, "checked") != NULL;
            b->d->links[link].width = 3;
            emit_field(b, link);
        }
    } else if (!strcmp(type, "submit") || !strcmp(type, "image")) {
        const char *label = value && *value ? value : attr(b, "alt") ? attr(b, "alt") : "";
        if (!*label)
            label = "Submit";
        link = field(b, WL_SUBMIT, label, false);
        if (link >= 0) {
            int n = (int)strlen(www_str(b->d, b->d->links[link].value));
            b->d->links[link].width = (uint8_t)(n + 2 > 40 ? 40 : n + 2);
            emit_field(b, link);
        }
    } else if (!strcmp(type, "button") || !strcmp(type, "reset") || !strcmp(type, "file")) {
        // nothing without scripts
    } else {
        link = field(b, !strcmp(type, "password") ? WL_PASSWORD : WL_TEXT, value, false);
        if (link >= 0) {
            int w = attr_int(b, "size", 20);
            b->d->links[link].width = (uint8_t)(w < 4 ? 4 : w > 40 ? 40 : w);
            emit_field(b, link);
        }
    }
}

/// Elements that are a paragraph of their own (an empty line before and after).
static const char *const para_tags[] = {
    "p", "h1", "h2", "h3", "h4", "h5", "h6", "pre", "blockquote", "ul", "ol", "dl", "table",
    "figure", "address", "fieldset", "details", "menu", "dir", "listing", "xmp", "plaintext", NULL,
};
/// Elements that start on a line of their own.
static const char *const line_tags[] = {
    "div", "section", "article", "header", "footer", "nav", "main", "aside", "center", "form",
    "tr", "li", "dt", "dd", "figcaption", "caption", "summary", "legend", "hgroup",
    "noscript", "iframe", "frame", "frameset", "body", "search", "textarea", NULL,
};

/// Whether the tag is in a list.
/// @param tag the tag
/// @param list the list, NULL at the end
/// @return it is
static bool in(const char *tag, const char *const *list) {
    for (; *list; ++list)
        if (!strcmp(tag, *list))
            return true;
    return false;
}

/// Whether the tag is a list.
/// @param tag the tag
/// @return it is
static bool is_list(const char *tag) {
    return !strcmp(tag, "ul") || !strcmp(tag, "ol") || !strcmp(tag, "menu") || !strcmp(tag, "dir");
}

/// Whether a list is open (so a list here is nested, with no empty lines round it).
/// @param b the state
/// @return one is
static bool in_list(const bld_t *b) {
    for (int i = 0; i < b->depth; ++i)
        if (is_list(b->blocks[i].tag))
            return true;
    return false;
}

/// A number attribute in pixels ("120" or "120px"), or 0 (missing, or "50%").
/// @param b the state
/// @param name the attribute
/// @return the pixels
static int attr_px(const bld_t *b, const char *name) {
    const char *v = attr(b, name);
    if (!v || !isdigit((unsigned char)*v))
        return 0;
    char *end;
    long n = strtol(v, &end, 10);
    return *end == '%' || n > 10000 ? 0 : (int)n;
}

/// An <img> in picture mode: a box of lines for it, its link, and its
/// [alt text] on the first line. False if it is shown as text only (no
/// address, a kind that can't be drawn, or a tiny icon).
/// @param b the state
/// @return it has a box
static bool picture(bld_t *b) {
    www_doc_t *d = b->d;
    const char *src = attr(b, "src"), *alt = attr(b, "alt");
    if ((!src || !*src || !strncmp(src, "data:", 5)) && attr(b, "data-src"))
        src = attr(b, "data-src");  // pictures loaded by a script
    if (!src || !*src || !strncmp(src, "data:", 5) || b->select >= 0 || b->pre)
        return false;
    size_t n = strcspn(src, "?#");
    if (n >= 4 && (!strncasecmp(src + n - 4, ".gif", 4) || !strncasecmp(src + n - 4, ".svg", 4) ||
                   (n >= 5 && !strncasecmp(src + n - 5, ".webp", 5))))
        return false;  // kinds that can't be drawn: their alt text
    int aw = attr_px(b, "width"), ah = attr_px(b, "height");
    if ((aw && aw <= 40 && (!ah || ah <= 40)) || (ah && ah <= 24))
        return false;  // an icon: as text
    int kw, kh;
    if (b->size_of && b->size_of(src, &kw, &kh)) {  // loaded already: its real size
        if (!kw || !kh)
            return false;  // it can't be drawn
        aw = kw;
        ah = kh;
    }
    // The box: the size asked for, made to fit; else a size for any picture.
    int ind = b->indent > MAX_INDENT ? MAX_INDENT : b->indent;
    int max_w = (WWW_COLS - ind) * 8, max_h = WWW_PIC_ROWS * 16;
    int w = aw ? aw : 400, h = ah ? ah : (aw ? aw * 3 / 4 : 240);
    if (w > max_w) {
        h = h * max_w / w;
        w = max_w;
    }
    if (h > max_h) {
        w = w * max_h / h;
        h = max_h;
    }
    if (w < 16)
        w = 16;
    if (h < 16)
        h = 16;
    if (!GROW(d, pics, d->npics + 1))
        return false;
    uint32_t at = pool_add(d, src, strlen(src));
    int link = b->link;
    if (link < 0 && (link = new_link(b, WL_IMAGE)) >= 0)
        d->links[link].href = at;
    if (link >= 0 && !d->links[link].img)
        d->links[link].img = at;
    // The box starts a line of its own, with the [alt text] on it.
    newline(b);
    int32_t line = d->nlines;  // (the line being filled, now empty)
    int was = b->link;
    b->link = link;
    b->link_cells = 0;
    snprintf(b->label, sizeof b->label, "[%s]", alt && *alt ? alt : "picture");
    text_utf8(b, b->label, WS_IMG);
    b->link = was;
    int rows = (h + 15) / 16;
    end_line(b);
    while (d->nlines < line + rows && !d->oom)
        end_line(b);  // the rest of the box: empty lines
    b->space = false;
    d->pics[d->npics++] = (www_pic_t){at, line, (uint8_t)rows, (uint8_t)ind, (uint16_t)w,
                                      (uint16_t)h, link};
    return true;
}

/// Whether the text at `s` is the tag `name` (after the '<' or "</").
/// @param s the text
/// @param end its end
/// @param name the tag's name
/// @return it is
static bool tag_is(const uint8_t *s, const uint8_t *end, const char *name) {
    size_t n = strlen(name);
    return s + n < end && !strncasecmp((const char *)s, name, n) && !isalnum(s[n]);
}

/// Whether a class attribute makes a <div> a drop-down menu: a class
/// "dropdown" or one ending "-dropdown" (Wikipedia's "vector-dropdown"), not
/// its parts ("dropdown-content", "dropdown-menu").
/// @param cls the classes, or NULL
/// @param n their length
/// @return it is
static bool dropdown_class(const char *cls, size_t n) {
    for (size_t i = 0; cls && i < n;) {
        while (i < n && cls[i] == ' ')
            ++i;
        size_t k = i;
        while (k < n && cls[k] != ' ' && cls[k] != '"' && cls[k] != '\'')
            ++k;
        size_t len = k - i;
        if ((len == 8 && !strncmp(cls + i, "dropdown", 8)) ||
            (len > 9 && !strncmp(cls + k - 9, "-dropdown", 9)))
            return true;
        if (k < n && (cls[k] == '"' || cls[k] == '\''))
            break;
        i = k;
    }
    return false;
}

/// Whether the raw tag at `s` (just after its '<') is a drop-down <div>.
/// @param s the tag
/// @param end the page's end
/// @return it is
static bool dropdown_div(const uint8_t *s, const uint8_t *end) {
    const uint8_t *gt = memchr(s, '>', end - s);
    if (!gt)
        return false;
    for (const uint8_t *c = s; c + 6 < gt; ++c)
        if (!strncasecmp((const char *)c, "class=", 6)) {
            const char *v = (const char *)c + 6 + (c[6] == '"' || c[6] == '\'');
            return dropdown_class(v, (size_t)((const char *)gt - v));
        }
    return false;
}

/// Skips a closed block to its end tag (nested ones of the same name
/// counted), counting the collapsible blocks inside it so the ones after keep
/// their numbers. Comments and scripts are skipped whole.
/// @param b the state
/// @param name the block's tag
static void skip_block(bld_t *b, const char *name) {
    int depth = 1;
    const uint8_t *s = b->p, *end = b->end;
    while (s < end) {
        if (*s != '<') {
            ++s;
            continue;
        }
        if (end - s >= 4 && !memcmp(s, "<!--", 4)) {
            const uint8_t *e = s + 4;
            while (e + 3 <= end && memcmp(e, "-->", 3))
                ++e;
            s = e + 3 <= end ? e + 3 : end;
            continue;
        }
        bool close = s + 1 < end && s[1] == '/';
        const uint8_t *t = s + 1 + close;
        if (!close && (tag_is(t, end, "script") || tag_is(t, end, "style"))) {
            const char *raw = tag_is(t, end, "script") ? "</script" : "</style";
            const uint8_t *e = t;
            while (e + 8 <= end && strncasecmp((const char *)e, raw, strlen(raw)))
                ++e;
            s = e;
            ++s;
            continue;
        }
        if (!close && (tag_is(t, end, "nav") || tag_is(t, end, "details") ||
                       (tag_is(t, end, "div") && dropdown_div(t, end))))
            ++b->ncoll;
        if (tag_is(t, end, name)) {
            depth += close ? -1 : 1;
            if (!depth) {
                const uint8_t *gt = memchr(s, '>', end - s);
                b->p = gt ? gt + 1 : end;
                return;
            }
        }
        ++s;
    }
    b->p = end;
}

/// A menu (<nav>) or <details>: a line "[+] name" that opens it (a WL_TOGGLE
/// link), and its contents only if it is open ("[-] name" then).
/// @param b the state
/// @return true if the block was skipped (closed), false to lay it out
static bool collapsible(bld_t *b) {
    www_doc_t *d = b->d;
    int id = b->ncoll++;
    bool details = IS("details"), div = IS("div");
    bool open = details && attr(b, "open");
    if (!b->toggled)
        open = true;
    else if (b->toggled(id))
        open = !open;
    // Its name: a <nav>'s aria-label, or the <summary> of a <details>.
    char name[80] = "";
    const char *label = attr(b, "aria-label");
    if (label && *label)
        to_cp437(label, name, sizeof name);
    if (div && !name[0]) {  // a drop-down: the text of its <label>, or its button's aria-label
        const uint8_t *s = b->p, *lim = b->p + 2000 < b->end ? b->p + 2000 : b->end;
        for (; s + 7 < lim; ++s)
            if (*s == '<' && tag_is(s + 1, lim, "label")) {
                const uint8_t *from = memchr(s, '>', lim - s), *to;
                if (!from)
                    break;
                ++from;
                for (to = from; to + 8 < lim && strncasecmp((const char *)to, "</label", 7); ++to) {
                }
                char raw[160];
                size_t n = 0;
                bool in_tag = false;
                for (const uint8_t *c = from; c < to && n + 1 < sizeof raw; ++c) {
                    if (*c == '<')
                        in_tag = true;
                    else if (*c == '>')
                        in_tag = false;
                    else if (!in_tag)
                        raw[n++] = (char)*c;
                }
                raw_text(b, (const uint8_t *)raw, (const uint8_t *)raw + n, name, sizeof name);
                break;
            }
    }
    if (details) {
        for (const uint8_t *s = b->p; s + 9 < b->end; ++s) {
            if (*s == '<' && (tag_is(s + 1, b->end, "details") || tag_is(s + 2, b->end, "details")))
                break;  // (a nested one, or the end: no summary of its own)
            if (*s == '<' && tag_is(s + 1, b->end, "summary")) {
                const uint8_t *from = memchr(s, '>', b->end - s), *to = from;
                if (!from)
                    break;
                ++from;
                for (to = from; to + 9 < b->end && strncasecmp((const char *)to, "</summary", 9); ++to) {
                }
                // The summary's text, without its tags.
                char raw[160];
                size_t n = 0;
                bool in_tag = false;
                for (const uint8_t *c = from; c < to && n + 1 < sizeof raw; ++c) {
                    if (*c == '<')
                        in_tag = true;
                    else if (*c == '>')
                        in_tag = false;
                    else if (!in_tag)
                        raw[n++] = (char)*c;
                }
                raw_text(b, (const uint8_t *)raw, (const uint8_t *)raw + n, name, sizeof name);
                break;
            }
        }
    }
    if (!name[0])
        snprintf(name, sizeof name, "%s", details ? "Details" : "Menu");
    newline(b);
    int link = new_link(b, WL_TOGGLE);
    if (link >= 0) {
        d->links[link].opts = (uint32_t)id;
        int was = b->link;
        b->link = link;
        b->link_cells = 0;
        put(b, '[', WS_BOLD, false, link);
        put(b, open ? '-' : '+', WS_BOLD, false, link);
        put(b, ']', WS_BOLD, false, link);
        put(b, ' ', WS_BOLD, false, link);
        for (const char *c = name; *c; ++c)
            put(b, (uint8_t)*c, WS_BOLD, *c == ' ', link);
        b->link = was;
    }
    newline(b);
    if (!open)
        skip_block(b, details ? "details" : div ? "div" : "nav");
    return !open;
}

/// A start tag.
/// @param b the state
static void start_tag(bld_t *b) {
    www_doc_t *d = b->d;
    if (b->select >= 0) {  // only options matter inside a <select>
        if (IS("option")) {
            end_option(b);
            b->in_opt = true;
            b->optlen = 0;
            b->opt_space = false;
            b->opt_sel = attr(b, "selected") != NULL;
            const char *v = attr(b, "value");
            b->has_optval = v != NULL;
            snprintf(b->optval, sizeof b->optval, "%s", v ? v : "");
        } else if (IS("select")) {
            end_select(b);
        }
        return;
    }
    const char *id = attr(b, "id");
    if (!id && IS("a"))
        id = attr(b, "name");
    if (id && GROW(d, anchors, d->nanchors + 1)) {
        d->anchors[d->nanchors].name = pool_add(d, id, strlen(id));
        d->anchors[d->nanchors++].line = d->nlines;
    }
    if ((IS("nav") || IS("details") ||
         (IS("div") && dropdown_class(attr(b, "class"), attr(b, "class") ? strlen(attr(b, "class")) : 0))) &&
        b->toggled && collapsible(b))
        return;  // (closed: skipped)
    if (IS("summary") && b->toggled) {  // (its text is the [-] line's)
        skip_to_end_tag(b, "summary", NULL);
        return;
    }
    if (in(b->tag, para_tags) && !(is_list(b->tag) && in_list(b)))
        para(b);
    else if (in(b->tag, line_tags) || is_list(b->tag))
        newline(b);
    if (b->tag[0] == 'h' && b->tag[1] >= '1' && b->tag[1] <= '6' && !b->tag[2]) {
        b->head = b->tag[1] - '0';
    } else if (IS("ul") || IS("ol") || IS("menu") || IS("dir")) {
        push_block(b, 3, IS("ol"));
    } else if (IS("li")) {
        int depth = 0;
        block_t *list = NULL;
        for (int i = b->depth - 1; i >= 0; --i)
            if (!strcmp(b->blocks[i].tag, "ul") || !strcmp(b->blocks[i].tag, "ol") ||
                !strcmp(b->blocks[i].tag, "menu") || !strcmp(b->blocks[i].tag, "dir")) {
                if (!list)
                    list = &b->blocks[i];
                ++depth;
            }
        if (list && list->ordered)
            snprintf(b->bullet, sizeof b->bullet, "%d.", list->count++);
        else
            snprintf(b->bullet, sizeof b->bullet, "%c", depth <= 1 ? 0x07 : depth == 2 ? 'o' : 0xfe);
    } else if (IS("blockquote")) {
        push_block(b, 4, false);
        ++b->quote;
    } else if (IS("dl")) {
        push_block(b, 0, false);
    } else if (IS("dt")) {
        if (b->depth && !strcmp(b->blocks[b->depth - 1].tag, "dd"))
            pop_block(b, "dd");
        b->dt = true;
    } else if (IS("dd")) {
        b->dt = false;
        if (b->depth && !strcmp(b->blocks[b->depth - 1].tag, "dd"))
            pop_block(b, "dd");
        push_block(b, 4, false);
    } else if (IS("pre") || IS("listing") || IS("xmp")) {
        ++b->pre;
        b->pre_start = true;
    } else if (IS("plaintext")) {
        ++b->pre;
        while (b->p < b->end)
            text(b, next_cp(b), WS_CODE);
    } else if (IS("hr")) {
        newline(b);
        int w = WWW_COLS - (b->indent > MAX_INDENT ? MAX_INDENT : b->indent);
        for (int i = 0; i < w; ++i)
            put(b, 0xc4, WS_RULE, false, -1);
        newline(b);
    } else if (IS("br")) {
        if (b->len > b->start)
            end_line(b);
        else if (b->blank < 2)
            end_line(b);
        b->space = false;
    } else if (IS("b") || IS("strong")) {
        ++b->bold;
    } else if (IS("i") || IS("em") || IS("cite") || IS("var") || IS("dfn")) {
        ++b->ital;
    } else if (IS("code") || IS("kbd") || IS("samp") || IS("tt")) {
        ++b->code;
    } else if (IS("q")) {
        text(b, '"', -1);
    } else if (IS("tr")) {
        b->cells_in_row = 0;
        b->cell_start = -1;
    } else if (IS("td") || IS("th")) {
        if (b->cells_in_row++ && b->len > b->start) {
            put(b, ' ', WS_TEXT, true, -1);
            put(b, ' ', WS_TEXT, true, -1);
        }
        b->space = false;
        b->cell_start = b->len;
        if (IS("th"))
            ++b->bold;
    } else if (IS("a")) {
        end_link(b);
        const char *href = attr(b, "href");
        if (href) {
            int link = new_link(b, WL_HREF);
            if (link >= 0) {
                d->links[link].href = pool_add(d, href, strlen(href));
                if (b->space && b->len > b->start)
                    put(b, ' ', WS_TEXT, true, -1);
                b->space = false;
                b->link = link;
                b->link_cells = 0;
            }
        }
    } else if (IS("img") && b->pictures && picture(b)) {
        // (drawn in the page)
    } else if (IS("img")) {
        // A picture shows its [alt text]. In a link, the link keeps its address
        // (the first one's); else, with alt text, it is a link of its own (to
        // view it). Without alt text, only an empty link shows it, as [IMG].
        const char *alt = attr(b, "alt"), *src = attr(b, "src");
        char buf[200];
        uint32_t at = src && *src ? pool_add(d, src, strlen(src)) : 0;
        if (b->link >= 0) {
            if (!d->links[b->link].img)
                d->links[b->link].img = at;
            snprintf(buf, sizeof buf, "[%s]", alt && *alt ? alt : "IMG");
            if ((alt && *alt) || b->link_cells == 0)
                text_utf8(b, buf, WS_IMG);
        } else if (alt && *alt) {
            int link = at ? new_link(b, WL_IMAGE) : -1;
            if (link >= 0) {
                d->links[link].href = d->links[link].img = at;
                if (b->space && b->len > b->start)
                    put(b, ' ', WS_TEXT, true, -1);
                b->space = false;
                b->link = link;
                b->link_cells = 0;
            }
            snprintf(buf, sizeof buf, "[%s]", alt);
            text_utf8(b, buf, WS_IMG);
            b->link = -1;
        }
    } else if (IS("iframe") || IS("frame")) {
        const char *src = attr(b, "src");
        if (src && *src) {
            const char *name = attr(b, "title") ? attr(b, "title") : attr(b, "name");
            snprintf(b->label, sizeof b->label, "%s: %s", IS("iframe") ? "IFRAME" : "FRAME",
                     name && *name ? name : src);
            link_line(b, src, b->label);
        }
    } else if (IS("meta")) {
        meta(b);
    } else if (IS("base")) {
        const char *href = attr(b, "href");
        if (href)
            snprintf(d->base, sizeof d->base, "%s", href);
    } else if (IS("form")) {
        if (GROW(d, forms, d->nforms + 1)) {
            const char *action = attr(b, "action"), *method = attr(b, "method");
            d->forms[d->nforms].action = pool_add(d, action ? action : "", action ? strlen(action) : 0);
            d->forms[d->nforms].post = method && !strcasecmp(method, "post");
            b->form = d->nforms++;
        }
    } else if (IS("input")) {
        input(b);
    } else if (IS("button")) {
        const char *t = attr(b, "type");
        if (!t || !strcasecmp(t, "submit")) {
            end_link(b);
            int link = field(b, WL_SUBMIT, attr(b, "value"), false);
            if (link >= 0) {
                if (b->space && b->len > b->start)
                    put(b, ' ', WS_TEXT, true, -1);
                b->space = false;
                b->link = link;
                b->link_cells = 0;
                put(b, '[', WS_FIELD, false, link);
            }
        }
    } else if (IS("select")) {
        int link = field(b, WL_SELECT, NULL, false);
        if (link >= 0) {
            b->select = link;
            b->in_opt = false;
        }
    } else if (IS("textarea")) {
        const uint8_t *from, *to = skip_to_end_tag(b, "textarea", &from);
        char value[256];
        raw_text(b, from, to, value, sizeof value);
        int link = field(b, WL_TEXTAREA, NULL, false);
        if (link >= 0) {
            d->links[link].value = pool_add(d, value, strlen(value));
            int w = attr_int(b, "cols", 40);
            d->links[link].width = (uint8_t)(w < 10 ? 10 : w > 40 ? 40 : w);
            emit_field(b, link);
        }
        newline(b);
    } else if (IS("title")) {
        const uint8_t *from, *to = skip_to_end_tag(b, "title", &from);
        if (!d->title[0])
            raw_text(b, from, to, d->title, sizeof d->title);
    } else if (IS("script") || IS("style") || IS("svg") || IS("template") || IS("math") ||
               IS("object") || IS("video") || IS("audio") || IS("canvas")) {
        skip_to_end_tag(b, b->tag, NULL);
    }
}

/// An end tag.
/// @param b the state
static void end_tag(bld_t *b) {
    if (b->select >= 0) {
        if (IS("option"))
            end_option(b);
        else if (IS("select"))
            end_select(b);
        return;
    }
    if (b->tag[0] == 'h' && b->tag[1] >= '1' && b->tag[1] <= '6' && !b->tag[2])
        b->head = 0;
    else if (IS("ul") || IS("ol") || IS("menu") || IS("dir") || IS("dl") || IS("dd"))
        pop_block(b, b->tag);
    else if (IS("blockquote")) {
        pop_block(b, "blockquote");
        if (b->quote)
            --b->quote;
    } else if (IS("dt"))
        b->dt = false;
    else if ((IS("pre") || IS("listing") || IS("xmp")) && b->pre)
        --b->pre;
    else if ((IS("b") || IS("strong")) && b->bold)
        --b->bold;
    else if ((IS("i") || IS("em") || IS("cite") || IS("var") || IS("dfn")) && b->ital)
        --b->ital;
    else if ((IS("code") || IS("kbd") || IS("samp") || IS("tt")) && b->code)
        --b->code;
    else if (IS("th") && b->bold)
        --b->bold;
    else if (IS("tr") || IS("table"))
        b->cell_start = -1;
    else if (IS("q"))
        text(b, '"', -1);
    else if (IS("a") && b->link >= 0 && b->d->links[b->link].type == WL_HREF)
        end_link(b);
    else if (IS("button") && b->link >= 0 && b->d->links[b->link].type == WL_SUBMIT)
        end_link(b);
    else if (IS("form"))
        b->form = -1;
    if (in(b->tag, para_tags) && !(is_list(b->tag) && in_list(b)))
        para(b);
    else if (in(b->tag, line_tags) || is_list(b->tag))
        newline(b);
}

/// Reads a tag at b->p (a '<'). False if it isn't one (the '<' is text).
/// @param b the state
/// @return it was a tag
static bool tag(bld_t *b) {
    const uint8_t *s = b->p + 1, *end = b->end;
    b->close = s < end && *s == '/';
    s += b->close;
    if (s >= end || !isalpha(*s))
        return false;
    int n = 0;
    while (s < end && (isalnum(*s) || *s == '-' || *s == ':')) {
        if (n < (int)sizeof b->tag - 1)
            b->tag[n++] = (char)tolower(*s);
        ++s;
    }
    b->tag[n] = 0;
    b->nattr = 0;
    int used = 0;
    for (;;) {
        while (s < end && (isspace(*s) || *s == '/'))
            ++s;
        if (s >= end || *s == '>')
            break;
        // The attribute's name.
        char *name = b->abuf + used;
        int nl = 0;
        while (s < end && !isspace(*s) && *s != '=' && *s != '>' && !(*s == '/' && nl)) {
            if (used + nl < ATTR_BUF - 2 && nl < 32)
                name[nl++] = (char)tolower(*s);
            ++s;
        }
        if (!nl) {  // a stray character
            ++s;
            continue;
        }
        name[nl] = 0;
        used += nl + 1;
        while (s < end && isspace(*s))
            ++s;
        char *val = b->abuf + (used < ATTR_BUF ? used : ATTR_BUF - 1);
        int vl = 0;
        if (s < end && *s == '=') {
            ++s;
            while (s < end && isspace(*s))
                ++s;
            uint8_t q = s < end && (*s == '"' || *s == '\'') ? *s++ : 0;
            while (s < end && (q ? *s != q : !isspace(*s) && *s != '>')) {
                uint32_t u;
                if (*s == '&') {
                    u = entity(&s, end);
                } else if (*s >= 0x80 && b->latin1) {
                    u = *s < 0xa0 ? cp1252[*s - 0x80] : *s;
                    ++s;
                } else {  // UTF-8 (or anything else) as it is
                    if (used + vl < ATTR_BUF - 2)
                        val[vl++] = (char)*s;
                    ++s;
                    continue;
                }
                if (u == '\n' || u == '\r' || u == '\t')
                    u = ' ';
                put_utf8(u, val, &vl, ATTR_BUF - 1 - used);
            }
            if (q && s < end)
                ++s;
        }
        val[vl] = 0;
        used += vl + 1;
        if (used >= ATTR_BUF - 2)
            used = ATTR_BUF - 2;
        if (b->nattr < MAX_ATTRS) {
            b->aname[b->nattr] = name;
            b->aval[b->nattr++] = val;
        }
    }
    b->p = s < end ? s + 1 : end;
    if (b->close)
        end_tag(b);
    else
        start_tag(b);
    return true;
}

// ---- The page -------------------------------------------------------------------------------------

www_doc_t *www_layout(const char *src, size_t len, bool html, bool latin1, bool pictures,
                      www_size_fn size_of, www_toggled_fn toggled) {
    www_doc_t *d = psram_malloc(sizeof *d);
    bld_t *b = psram_malloc(sizeof *b);
    if (!d || !b) {
        psram_free(d);
        psram_free(b);
        return NULL;
    }
    memset(d, 0, sizeof *d);
    memset(b, 0, sizeof *b);
    pool_add(d, "", 0);  // offset 0 is ""
    b->d = d;
    b->p = (const uint8_t *)src;
    b->end = b->p + len;
    b->latin1 = latin1;
    b->pictures = pictures;
    b->size_of = size_of;
    b->toggled = toggled;
    b->link = b->form = b->select = b->cell_start = -1;
    if (len >= 3 && !memcmp(src, "\xef\xbb\xbf", 3)) {
        b->p += 3;
        b->latin1 = false;
    }
    if (!html)
        b->pre = 1;
    while (b->p < b->end && !d->oom) {
        uint8_t c = *b->p;
        if (html && c == '<') {
            const uint8_t *s = b->p;
            if (b->end - s >= 4 && !memcmp(s, "<!--", 4)) {
                const uint8_t *e = s + 4;
                while (e + 3 <= b->end && memcmp(e, "-->", 3))
                    ++e;
                b->p = e + 3 <= b->end ? e + 3 : b->end;
                continue;
            }
            if (b->end - s >= 2 && (s[1] == '!' || s[1] == '?')) {
                const uint8_t *gt = memchr(s, '>', b->end - s);
                b->p = gt ? gt + 1 : b->end;
                continue;
            }
            if (tag(b))
                continue;
            ++b->p;
            text(b, '<', -1);
            continue;
        }
        if (html && c == '&') {
            text(b, entity(&b->p, b->end), -1);
            continue;
        }
        text(b, next_cp(b), -1);
    }
    end_select(b);
    end_link(b);
    newline(b);
    while (d->nlines && d->lines[d->nlines - 1].len == 0)  // no empty lines at the end
        --d->nlines;
    psram_free(b);
    return d;
}

void www_free(www_doc_t *d) {
    if (!d)
        return;
    psram_free(d->cells);
    psram_free(d->lines);
    psram_free(d->segs);
    psram_free(d->links);
    psram_free(d->forms);
    psram_free(d->anchors);
    psram_free(d->pics);
    psram_free(d->pool);
    psram_free(d);
}

int32_t www_seg_from(const www_doc_t *d, int32_t line) {
    int32_t lo = 0, hi = d->nsegs;
    while (lo < hi) {
        int32_t mid = (lo + hi) / 2;
        if (d->segs[mid].line < line)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

int32_t www_anchor(const www_doc_t *d, const char *name) {
    for (int32_t i = 0; i < d->nanchors; ++i)
        if (!strcmp(www_str(d, d->anchors[i].name), name))
            return d->anchors[i].line;
    return -1;
}

// ---- URLs and forms -------------------------------------------------------------------------------

/// Takes "." and ".." segments out of a path (in place; up to ? or #).
/// @param path the path, from its '/' (room for WWW_URL_MAX)
/// @param scratch room for 2 * WWW_URL_MAX bytes
static void remove_dots(char *path, char *scratch) {
    char *rest = scratch, *out = scratch + WWW_URL_MAX;
    size_t plen = strcspn(path, "?#");
    snprintf(rest, WWW_URL_MAX, "%s", path + plen);
    path[plen] = 0;
    size_t n = 0;
    const char *s = path;
    while (*s) {
        if (!strncmp(s, "/./", 3)) {
            s += 2;
        } else if (!strcmp(s, "/.")) {
            out[n++] = '/';
            break;
        } else if (!strncmp(s, "/../", 4) || !strcmp(s, "/..")) {
            while (n > 0 && out[--n] != '/') {
            }
            if (s[3] == 0) {
                out[n++] = '/';
                break;
            }
            s += 3;
        } else {
            do
                if (n < WWW_URL_MAX - 1)
                    out[n++] = *s++;
                else
                    ++s;
            while (*s && *s != '/');
        }
    }
    out[n] = 0;
    snprintf(path, WWW_URL_MAX, "%s%s", n ? out : "/", rest);
}

void www_resolve(const char *base, const char *rel, char *out, size_t size) {
    // Room for the link, the joined URL, its path and remove_dots (PSRAM:
    // the stack is small).
    struct {
        char r[WWW_URL_MAX], tmp[WWW_URL_MAX], path[WWW_URL_MAX], scratch[2 * WWW_URL_MAX];
    } *w = psram_malloc(sizeof *w);
    if (!w) {
        snprintf(out, size, "%s", rel);
        return;
    }
    char *r = w->r, *tmp = w->tmp, *path = w->path, *scratch = w->scratch;
    while (*rel == ' ' || *rel == '\t' || *rel == '\n' || *rel == '\r')
        ++rel;
    size_t rn = 0;
    for (const char *s = rel; *s && rn < WWW_URL_MAX - 1; ++s)  // no newlines; trailing spaces off
        if (*s != '\n' && *s != '\r' && *s != '\t')
            r[rn++] = *s;
    while (rn && r[rn - 1] == ' ')
        --rn;
    r[rn] = 0;
    // The base's scheme and host ("http://host"), and where its path ends.
    const char *colon = strstr(base, "://");
    size_t auth_end = colon ? (size_t)(colon + 3 - base) + strcspn(colon + 3, "/?#") : 0;
    size_t path_end = auth_end + strcspn(base + auth_end, "?#");
    size_t sch = strspn(r, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789+-.");
    if (sch && r[sch] == ':' && isalpha((unsigned char)r[0])) {
        snprintf(tmp, WWW_URL_MAX, "%s", r);  // absolute already
    } else if (r[0] == '/' && r[1] == '/') {
        snprintf(tmp, WWW_URL_MAX, "%.*s%s", colon ? (int)(colon - base + 1) : 0, base, r);
    } else if (!r[0] || r[0] == '#') {
        snprintf(tmp, WWW_URL_MAX, "%.*s%s", (int)strcspn(base, "#"), base, r);
    } else if (r[0] == '?') {
        snprintf(tmp, WWW_URL_MAX, "%.*s%s%s", (int)path_end, base,
                 path_end == auth_end ? "/" : "", r);
    } else if (r[0] == '/') {
        snprintf(tmp, WWW_URL_MAX, "%.*s%s", (int)auth_end, base, r);
    } else {
        size_t dir = path_end;
        while (dir > auth_end && base[dir - 1] != '/')
            --dir;
        snprintf(tmp, WWW_URL_MAX, "%.*s%s%s", (int)dir, base, dir == auth_end ? "/" : "", r);
    }
    // Dots out of the path, for http, https and file URLs.
    const char *h = strstr(tmp, "://");
    if (h && (!strncasecmp(tmp, "http", 4) || !strncasecmp(tmp, "file:", 5))) {
        size_t at = (size_t)(h + 3 - tmp) + strcspn(h + 3, "/?#");
        if (tmp[at] == '/') {
            snprintf(path, WWW_URL_MAX, "%s", tmp + at);
            remove_dots(path, scratch);
            snprintf(tmp + at, WWW_URL_MAX - at, "%s", path);
        }
    }
    snprintf(out, size, "%s", tmp);
    psram_free(w);
}

/// Percent-encodes text as UTF-8 for a form or query.
/// @param s the text
/// @param utf8 it is UTF-8 already (else CP437)
/// @param out the output
/// @param n its length so far (moved on)
/// @param size room in out
/// @return it fitted
static bool encode(const char *s, bool utf8, char *out, size_t *n, size_t size) {
    static const char hex[] = "0123456789ABCDEF";
    for (; *s; ++s) {
        char u8[4];
        int k = 0;
        if (utf8 || (uint8_t)*s < 0x80)
            u8[k++] = *s;
        else
            put_utf8(www_unicode((uint8_t)*s), u8, &k, sizeof u8 + 1);
        for (int i = 0; i < k; ++i) {
            uint8_t c = (uint8_t)u8[i];
            if (*n + 4 > size)
                return false;
            if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
                out[(*n)++] = (char)c;
            } else if (c == ' ') {
                out[(*n)++] = '+';
            } else {
                out[(*n)++] = '%';
                out[(*n)++] = hex[c >> 4];
                out[(*n)++] = hex[c & 15];
            }
        }
    }
    out[*n] = 0;
    return true;
}

bool www_url_encode(const char *s, char *out, size_t size) {
    size_t n = 0;
    out[0] = 0;
    return encode(s, false, out, &n, size);
}

bool www_form_data(const www_doc_t *d, int form, int button, char *out, size_t size) {
    size_t n = 0;
    out[0] = 0;
    for (int i = 0; i < d->nlinks; ++i) {
        const www_link_t *l = &d->links[i];
        if (l->form != form || l->type == WL_HREF || l->type == WL_IMAGE || l->type == WL_TOGGLE)
            continue;
        const char *name = www_str(d, l->href);
        if (!name[0])
            continue;
        const char *value = www_str(d, l->value);
        bool utf8 = false;
        switch (l->type) {
        case WL_CHECK:
        case WL_RADIO:
            if (!l->checked)
                continue;
            utf8 = true;
            break;
        case WL_HIDDEN:
            utf8 = true;
            break;
        case WL_SELECT:
            if (!l->nopts)
                continue;
            value = www_option(d, i, l->sel, true);
            utf8 = true;
            break;
        case WL_SUBMIT:
            if (i != button)
                continue;
            break;
        }
        if (n && n + 1 < size)
            out[n++] = '&';
        if (!encode(name, true, out, &n, size) || n + 2 > size)
            return false;
        out[n++] = '=';
        out[n] = 0;
        if (!encode(value, utf8, out, &n, size))
            return false;
    }
    return true;
}
