/**
 * @file tts.c
 * @brief The text-to-speech engine (tts.h): text to phonemes, and phonemes to 16 kHz speech with a formant synthesizer.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */
#include "tts.h"

#include <ctype.h>
#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "tts_rules.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// ---- Phonemes ---------------------------------------------------------------------------------------

/// The kinds of phoneme.
enum { K_VOWEL, K_DIPH, K_SONOR, K_NASAL, K_FRIC, K_STOP, K_AFFR, K_ASP };

/// One phoneme: its formant targets and sources. Frequencies in Hz, times in
/// ms, levels in percent.
typedef struct {
    char name[3];     ///< its name ("AA")
    uint8_t kind;     ///< K_ value
    uint8_t voiced;   ///< the vocal folds buzz (consonants)
    uint8_t dur;      ///< its length (a stop's: the closure's)
    uint16_t f[3];    ///< the first three formants (a diphthong's start)
    uint16_t g[3];    ///< a diphthong's end formants
    uint8_t av;       ///< voicing's level
    uint8_t af;       ///< frication's level (a stop's: the burst's)
    uint16_t fc, fb;  ///< frication's centre and width
} ph_t;

#define V(n, f1, f2, f3, d) {n, K_VOWEL, 1, d, {f1, f2, f3}, {0, 0, 0}, 100, 0, 0, 0}
#define D(n, f1, f2, f3, g1, g2, g3, d) {n, K_DIPH, 1, d, {f1, f2, f3}, {g1, g2, g3}, 100, 0, 0, 0}
#define S(n, f1, f2, f3, d) {n, K_SONOR, 1, d, {f1, f2, f3}, {0, 0, 0}, 75, 0, 0, 0}
#define N(n, f1, f2, f3, d) {n, K_NASAL, 1, d, {f1, f2, f3}, {0, 0, 0}, 60, 0, 0, 0}
#define F(n, vo, f1, f2, f3, d, av, af, fc, fb) {n, K_FRIC, vo, d, {f1, f2, f3}, {0, 0, 0}, av, af, fc, fb}
#define P(n, vo, f1, f2, f3, d, af, fc, fb) {n, K_STOP, vo, d, {f1, f2, f3}, {0, 0, 0}, 0, af, fc, fb}

/// The phonemes (formants after Peterson and Barney's and Klatt's tables, for
/// a man's voice).
static const ph_t phons[] = {
    V("IY", 270, 2290, 3010, 120), V("IH", 390, 1990, 2550, 85), V("EH", 530, 1840, 2480, 105),
    V("AE", 660, 1720, 2410, 140), V("AA", 730, 1090, 2440, 145), V("AO", 570, 840, 2410, 145),
    V("UH", 440, 1020, 2240, 90), V("UW", 300, 870, 2240, 125), V("AH", 640, 1190, 2390, 100),
    V("ER", 490, 1350, 1690, 130), V("AX", 500, 1400, 2300, 55),
    D("EY", 480, 1720, 2520, 330, 2200, 2800, 155), D("AY", 700, 1220, 2600, 400, 1950, 2600, 180),
    D("OY", 550, 960, 2400, 400, 1900, 2600, 195), D("AW", 720, 1250, 2550, 450, 900, 2300, 185),
    D("OW", 520, 1000, 2400, 400, 850, 2300, 155),
    S("W", 290, 610, 2150, 65), S("Y", 260, 2070, 3020, 60), S("R", 310, 1060, 1380, 65),
    S("L", 330, 1050, 2880, 65),
    N("M", 270, 1000, 2200, 70), N("N", 270, 1500, 2500, 60), N("NG", 270, 2000, 2800, 75),
    F("F", 0, 340, 1100, 2080, 100, 0, 22, 6500, 5000), F("V", 1, 220, 1100, 2080, 60, 55, 14, 6500, 5000),
    F("TH", 0, 320, 1290, 2540, 90, 0, 18, 6000, 5000), F("DH", 1, 270, 1290, 2540, 50, 55, 10, 6000, 5000),
    F("S", 0, 320, 1390, 2530, 105, 0, 70, 5500, 2500), F("Z", 1, 240, 1390, 2530, 75, 45, 50, 5500, 2500),
    F("SH", 0, 300, 1840, 2750, 105, 0, 70, 2800, 1600), F("ZH", 1, 300, 1840, 2750, 70, 45, 50, 2800, 1600),
    P("P", 0, 400, 1100, 2150, 75, 45, 1200, 2000), P("B", 1, 200, 1100, 2150, 65, 30, 1200, 2000),
    P("T", 0, 400, 1600, 2600, 65, 60, 4200, 2500), P("D", 1, 200, 1700, 2600, 55, 40, 4200, 2500),
    P("K", 0, 300, 1990, 2850, 70, 60, 2300, 1200), P("G", 1, 200, 1990, 2850, 60, 40, 2300, 1200),
    {"CH", K_AFFR, 0, 65, {400, 1700, 2650}, {0, 0, 0}, 0, 0, 0, 0},
    {"JH", K_AFFR, 1, 55, {200, 1700, 2650}, {0, 0, 0}, 0, 0, 0, 0},
    {"HH", K_ASP, 0, 60, {0, 0, 0}, {0, 0, 0}, 0, 0, 0, 0},
};
#define NPH ((int)(sizeof phons / sizeof phons[0]))

/// The phoneme with name `s` (`n` letters long), or -1.
/// @param s the name
/// @param n its length
/// @return its index in phons, or -1
static int ph_find(const char *s, int n) {
    for (int i = 0; i < NPH; ++i)
        if ((int)strlen(phons[i].name) == n && !strncmp(phons[i].name, s, n))
            return i;
    return -1;
}

// ---- Text to phonemes -------------------------------------------------------------------------------

/// The output being built by tts_phonemes.
typedef struct {
    char *s;       ///< the string
    size_t size;   ///< its room
    size_t len;    ///< its length
    bool full;     ///< something didn't fit
} buf_t;

/// Adds text to the output (nothing more once anything hasn't fitted).
/// @param b the output
/// @param s the text
static void put(buf_t *b, const char *s) {
    size_t n = strlen(s);
    if (b->full || b->len + n + 1 > b->size) {
        b->full = true;
        return;
    }
    memcpy(b->s + b->len, s, n + 1);
    b->len += n;
}

/// The letter at `p` in the word, or a space past its ends.
static char at(const char *w, int n, int p) {
    return p >= 0 && p < n ? w[p] : ' ';
}

/// A vowel letter (A E I O U).
static bool is_vowel(char c) { return c && strchr("AEIOU", c); }
/// A consonant letter (Y included).
static bool is_cons(char c) { return c >= 'A' && c <= 'Z' && !is_vowel(c); }

/// Whether a rule's left context fits the letters before position `p`.
/// @param w the word (with a space at each end)
/// @param n its length
/// @param p the position just before the match
/// @param ctx the context
/// @return it fits
static bool fit_left(const char *w, int n, int p, const char *ctx) {
    for (int k = (int)strlen(ctx) - 1; k >= 0; --k) {
        char c = ctx[k], h = at(w, n, p);
        switch (c) {
        case ' ':
            if (isalpha((unsigned char)h))
                return false;
            --p;
            break;
        case '#':
            if (!is_vowel(h))
                return false;
            while (is_vowel(at(w, n, p)))
                --p;
            break;
        case ':':
            while (is_cons(at(w, n, p)))
                --p;
            break;
        case '^':
            if (!is_cons(h))
                return false;
            --p;
            break;
        case '.':
            if (!h || !strchr("BDVGJLMNRWZ", h))
                return false;
            --p;
            break;
        case '+':
            if (!h || !strchr("EIY", h))
                return false;
            --p;
            break;
        case '&':
        case '@':
            if (h == 'H' && strchr(c == '&' ? "CS" : "TCS", at(w, n, p - 1)))
                p -= 2;
            else if (h != ' ' && strchr(c == '&' ? "SCGZXJ" : "TSRDLZNJ", h))
                --p;
            else
                return false;
            break;
        default:
            if (h != c)
                return false;
            --p;
        }
    }
    return true;
}

/// Whether a rule's right context fits the letters from position `p`.
/// @param w the word (with a space at each end)
/// @param n its length
/// @param p the position just after the match
/// @param ctx the context
/// @return it fits
static bool fit_right(const char *w, int n, int p, const char *ctx) {
    for (; *ctx; ++ctx) {
        char c = *ctx, h = at(w, n, p);
        switch (c) {
        case ' ':
            if (isalpha((unsigned char)h))
                return false;
            ++p;
            break;
        case '#':
            if (!is_vowel(h))
                return false;
            while (is_vowel(at(w, n, p)))
                ++p;
            break;
        case ':':
            while (is_cons(at(w, n, p)))
                ++p;
            break;
        case '^':
            if (!is_cons(h))
                return false;
            ++p;
            break;
        case '.':
            if (!h || !strchr("BDVGJLMNRWZ", h))
                return false;
            ++p;
            break;
        case '+':
            if (!h || !strchr("EIY", h))
                return false;
            ++p;
            break;
        case '&':
        case '@':
            if (strchr(c == '&' ? "CS" : "TCS", h) && at(w, n, p + 1) == 'H')
                p += 2;
            else if (h != ' ' && strchr(c == '&' ? "SCGZXJ" : "TSRDLZNJ", h))
                ++p;
            else
                return false;
            break;
        case '%': {
            static const char *const ends[] = {"ING", "ELY", "ER", "ES", "ED", "E"};
            int k;
            for (k = 0; k < 6; ++k) {
                int m = (int)strlen(ends[k]);
                if (p + m <= n && !strncmp(w + p, ends[k], m)) {
                    p += m;
                    break;
                }
            }
            if (k == 6)
                return false;
            break;
        }
        default:
            if (h != c)
                return false;
            ++p;
        }
    }
    return true;
}

/// Words that are said without stress (they lean on the words around them).
static const char *const light_words[] = {
    "A", "AN", "THE", "OF", "TO", "AND", "OR", "BUT", "IN", "ON", "AT", "BY", "FOR", "WITH",
    "FROM", "AS", "IS", "AM", "ARE", "WAS", "WERE", "BE", "BEEN", "IT", "ITS", "HE", "SHE",
    "WE", "THEY", "YOU", "ME", "MY", "YOUR", "HIS", "HER", "OUR", "THEIR", "THEM", "US", "HAS",
    "HAVE", "HAD", "CAN", "WILL", "WOULD", "SHOULD", "COULD", "THAN", "THAT", "IF", "SO",
};

/// Words the rules get wrong, and how they are said.
static const char *const odd_words[][2] = {
    {"PICO", "P IY1 K OW"}, {"OS", "OW1 EH1 S"}, {"WHO", "HH UW1"}, {"WORLD", "W ER1 L D"},
    {"COMPUTER", "K AX M P Y UW1 T ER"}, {"HELLO", "HH AX L OW1"}, {"SPEECH", "S P IY1 CH"},
    {"VOICE", "V OY1 S"}, {"TWO", "T UW1"}, {"EIGHT", "EY1 T"}, {"SAYS", "S EH1 Z"},
    {"WOMAN", "W UH1 M AX N"}, {"WOMEN", "W IH1 M IH N"}, {"BUSY", "B IH1 Z IY"},
    {"LIVE", "L IH1 V"}, {"GIVE", "G IH1 V"}, {"HAVE", "HH AE V"}, {"MANY", "M EH1 N IY"},
    {"ANY", "EH1 N IY"}, {"FOUR", "F AO1 R"}, {"YOUR", "Y AO R"}, {"SIX", "S IH1 K S"},
    {"ZERO", "Z IY1 R OW"}, {"KEYBOARD", "K IY1 B AO R D"}, {"DRIVE", "D R AY1 V"},
    {"FILE", "F AY1 L"}, {"HUNDRED", "HH AH1 N D R AX D"}, {"VIDEO", "V IH1 D IY OW"}, {"AUDIO", "AO1 D IY OW"},
};

/// How each letter is said when a word is spelled.
static const char *const letter_names[26] = {
    "EY1", "B IY1", "S IY1", "D IY1", "IY1", "EH1 F", "JH IY1", "EY1 CH", "AY1", "JH EY1",
    "K EY1", "EH1 L", "EH1 M", "EH1 N", "OW1", "P IY1", "K Y UW1", "AA1 R", "EH1 S", "T IY1",
    "Y UW1", "V IY1", "D AH1 B AX L Y UW", "EH1 K S", "W AY1", "Z IY1",
};

/// Adds one word's phonemes (by the rules), stressing its first full vowel
/// unless it is a light word, then "/".
/// @param b the output
/// @param word the word, in capitals
static void say_word(buf_t *b, const char *word) {
    for (size_t i = 0; i < sizeof odd_words / sizeof odd_words[0]; ++i)
        if (!strcmp(word, odd_words[i][0])) {
            put(b, odd_words[i][1]);
            put(b, " / ");
            return;
        }
    bool light = false;
    for (size_t i = 0; i < sizeof light_words / sizeof light_words[0]; ++i)
        light |= !strcmp(word, light_words[i]);

    // The rules' phonemes into ph[], then stress one vowel.
    char w[64], ph[256];
    int n = (int)strlen(word);
    if (n > 60)
        n = 60;
    w[0] = ' ';
    memcpy(w + 1, word, n);
    w[n + 1] = ' ';
    n += 2;
    w[n] = 0;
    size_t pl = 0;
    ph[0] = 0;
    for (int p = 1; p < n - 1;) {
        if (p > 1 && w[p] == w[p - 1] && is_cons(w[p]) && w[p] != 'C') {
            ++p;  // a doubled consonant says once ("apple")
            continue;
        }
        const tts_rule_t *r = tts_rules;
        for (; r->match; ++r) {
            int m = (int)strlen(r->match);
            if (r->match[0] == w[p] && p + m <= n && !strncmp(w + p, r->match, m) &&
                fit_left(w, n, p - 1, r->left) && fit_right(w, n, p + m, r->right))
                break;
        }
        if (!r->match) {  // a letter no rule has (none should be)
            ++p;
            continue;
        }
        size_t ol = strlen(r->out);
        if (ol && pl + ol + 2 < sizeof ph) {
            memcpy(ph + pl, r->out, ol);
            pl += ol;
            ph[pl++] = ' ';
            ph[pl] = 0;
        }
        p += (int)strlen(r->match);
    }
    if (!light) {
        // The first vowel other than a schwa; or a schwa if that's all there is.
        char *best = NULL;
        for (char *s = ph; *s; s = strchr(s, ' ') + 1) {
            int k = ph_find(s, (int)(strchr(s, ' ') - s));
            if (k >= 0 && (phons[k].kind == K_VOWEL || phons[k].kind == K_DIPH)) {
                if (strncmp(s, "AX ", 3)) {
                    best = s;
                    break;
                }
                if (!best)
                    best = s;
            }
        }
        if (best) {
            char *e = strchr(best, ' ');
            if (pl + 2 < sizeof ph) {
                memmove(e + 1, e, strlen(e) + 1);
                *e = '1';
                ++pl;
            }
        }
    }
    put(b, ph);
    put(b, "/ ");
}

/// Spells a word letter by letter.
/// @param b the output
/// @param word the word (letters)
static void spell(buf_t *b, const char *word) {
    for (; *word; ++word)
        if (isalpha((unsigned char)*word)) {
            put(b, letter_names[toupper((unsigned char)*word) - 'A']);
            put(b, " / ");
        }
}

/// The numbers below twenty, and the tens, as words.
static const char *const small_nums[20] = {
    "ZERO", "ONE", "TWO", "THREE", "FOUR", "FIVE", "SIX", "SEVEN", "EIGHT", "NINE", "TEN",
    "ELEVEN", "TWELVE", "THIRTEEN", "FOURTEEN", "FIFTEEN", "SIXTEEN", "SEVENTEEN", "EIGHTEEN",
    "NINETEEN",
};
static const char *const tens_nums[10] = {
    "", "", "TWENTY", "THIRTY", "FORTY", "FIFTY", "SIXTY", "SEVENTY", "EIGHTY", "NINETY",
};

/// Says a number below a thousand (not zero).
/// @param b the output
/// @param v the number (1-999)
static void say_hundreds(buf_t *b, unsigned v) {
    if (v >= 100) {
        say_word(b, small_nums[v / 100]);
        say_word(b, "HUNDRED");
        v %= 100;
    }
    if (v >= 20) {
        say_word(b, tens_nums[v / 10]);
        v %= 10;
    }
    if (v)
        say_word(b, small_nums[v]);
}

/// Says a whole number as words ("one thousand two hundred").
/// @param b the output
/// @param v the number
static void say_number(buf_t *b, unsigned long v) {
    if (!v) {
        say_word(b, "ZERO");
        return;
    }
    static const char *const groups[] = {"BILLION", "MILLION", "THOUSAND"};
    static const unsigned long sizes[] = {1000000000UL, 1000000UL, 1000UL};
    for (int i = 0; i < 3; ++i)
        if (v >= sizes[i]) {
            say_hundreds(b, (unsigned)(v / sizes[i]));
            say_word(b, groups[i]);
            v %= sizes[i];
        }
    if (v)
        say_hundreds(b, (unsigned)v);
}

/// Adds a pause mark, unless one is already last (a stronger one replaces it).
/// @param b the output
/// @param mark ',' '.' or '?'
static void pause(buf_t *b, char mark) {
    size_t l = b->len;
    while (l && (b->s[l - 1] == ' ' || b->s[l - 1] == '/'))
        --l;
    if (!l)
        return;  // nothing said yet
    char last = b->s[l - 1];
    if (last == ',' || last == '.' || last == '?') {
        if (last != ',' || mark == ',')
            return;
        b->len = l - 1;
        b->s[b->len] = 0;
        b->full = false;
    }
    char m[4] = {mark, ' ', 0};
    put(b, m);
}

int tts_phonemes(const char *text, char *out, size_t size) {
    buf_t b = {out, size, 0, false};
    if (size)
        out[0] = 0;
    // Capitals are spelled ("USB") when the text also has small letters; in
    // all-capital text, only words with no vowels are.
    bool lower = false;
    for (const char *s = text; *s; ++s)
        lower |= islower((unsigned char)*s) != 0;
    size_t good = 0;  // the output up to the last whole word
    for (const char *s = text; *s && !b.full;) {
        unsigned char c = (unsigned char)*s;
        good = b.len;
        if (c == '[') {  // phonemes as they are
            const char *e = strchr(s, ']');
            if (!e)
                e = s + strlen(s);
            char ph[256];
            size_t n = (size_t)(e - s - 1) < sizeof ph - 1 ? (size_t)(e - s - 1) : sizeof ph - 1;
            for (size_t i = 0; i < n; ++i)
                ph[i] = (char)toupper((unsigned char)s[1 + i]);
            ph[n] = 0;
            put(&b, ph);
            put(&b, " / ");
            s = *e ? e + 1 : e;
        } else if (isalpha(c) || (c == '\'' && isalpha((unsigned char)s[1]))) {
            char w[64];
            size_t n = 0;
            bool caps = true, vowel = false;
            for (; isalpha((unsigned char)*s) || (*s == '\'' && isalpha((unsigned char)s[1])); ++s)
                if (n < sizeof w - 1) {
                    w[n] = (char)toupper((unsigned char)*s);
                    caps &= !islower((unsigned char)*s);
                    vowel |= strchr("AEIOUY", w[n]) != NULL;
                    ++n;
                }
            w[n] = 0;
            bool known = false;
            for (size_t i = 0; i < sizeof odd_words / sizeof odd_words[0]; ++i)
                known |= !strcmp(w, odd_words[i][0]) && strcmp(w, "OS");
            if (caps && n >= 2 && n <= 5 && !strchr(w, '\'') && !known && (lower || !vowel))
                spell(&b, w);
            else
                say_word(&b, w);
        } else if (isdigit(c)) {
            unsigned long v = 0;
            int digits = 0;
            const char *d0 = s;
            for (; isdigit((unsigned char)*s) || (*s == ',' && isdigit((unsigned char)s[1]) && s > d0);
                 ++s)
                if (*s != ',') {
                    if (digits < 9)
                        v = v * 10 + (unsigned long)(*s - '0');
                    ++digits;
                }
            if (digits > 9 || (digits > 1 && *d0 == '0')) {
                for (const char *t = d0; t < s; ++t)  // a long one, digit by digit
                    if (isdigit((unsigned char)*t))
                        say_word(&b, small_nums[*t - '0']);
            } else
                say_number(&b, v);
            if (*s == '.' && isdigit((unsigned char)s[1])) {
                say_word(&b, "POINT");
                for (++s; isdigit((unsigned char)*s); ++s)
                    say_word(&b, small_nums[*s - '0']);
            }
        } else {
            static const char *const signs[][2] = {
                {"&", "AND"}, {"+", "PLUS"}, {"%", "PERCENT"}, {"@", "AT"}, {"=", "EQUALS"},
                {"$", "DOLLARS"}, {"*", "TIMES"}, {"<", "LESS THAN"}, {">", "MORE THAN"},
            };
            if (c == ',' || c == ';' || c == ':' || c == '(' || c == ')' || c == '"' ||
                (c == '-' && s[1] == ' '))
                pause(&b, ',');
            else if (c == '.' || c == '!')
                pause(&b, '.');
            else if (c == '?')
                pause(&b, '?');
            else
                for (size_t i = 0; i < sizeof signs / sizeof signs[0]; ++i)
                    if (c == signs[i][0][0]) {
                        char w[16];
                        strcpy(w, signs[i][1]);
                        for (char *t = strtok(w, " "); t; t = strtok(NULL, " "))
                            say_word(&b, t);
                    }
            ++s;
        }
    }
    if (b.full) {
        b.len = good;
        if (size)
            out[good] = 0;
        return -1;
    }
    pause(&b, '.');
    if (b.full) {
        if (size)
            out[good] = 0;
        return -1;
    }
    return (int)b.len;
}

// ---- The synthesizer --------------------------------------------------------------------------------

#define FS ((float)TTS_RATE)
#define FRAME 64     ///< samples between parameter updates (4 ms)
#define OPEN 0.6f    ///< the open part of each glottal period
#define AMP_TR 5     ///< ms over which the sources' levels change
#define PITCH_TR 60  ///< ms over which the pitch moves

// The sources' gains: set so the loudest vowels peak near full scale.
#define G_VOICE 0.005f
#define G_ASP 0.028f
#define G_FRIC 0.5f

/// What the synthesizer aims at through one stretch of sound. The levels are
/// 0-1; the pitch is a multiple of the voice's own.
typedef struct {
    uint32_t start, len;  ///< where it is, in samples
    uint16_t tr;          ///< samples over which the formants glide in and out
    float f[3], b[3];     ///< formants and their widths
    float av, ah, af;     ///< voicing, aspiration and frication
    float fc, fb;         ///< frication's centre and width
    float f0;             ///< the pitch
    uint8_t vowel;        ///< it is (part of) a vowel
    uint8_t stress;       ///< a stressed vowel
} seg_t;

/// The parameters blended from the segments for one moment.
typedef struct {
    float f[3], b[3], av, ah, af, fc, fb, f0;
} par_t;

/// A second-order resonator.
typedef struct {
    float a, b, c, y1, y2;
} res_t;

/// The synthesizer's state.
struct tts {
    seg_t *seg;          ///< the stretches of sound
    int nseg;            ///< how many
    uint32_t total;      ///< the speech's length in samples
    uint32_t pos;        ///< the next sample
    int cur;             ///< the segment pos is in
    float base;          ///< the voice's pitch (Hz)
    res_t r[5];          ///< the cascade: F1-F3, and F4 and F5 fixed
    float fx1, fx2, fy1, fy2;   ///< frication filter's history
    float fb0, fa1, fa2;        ///< and its coefficients (band-pass, peak 1)
    float phase, f0, tilt;      ///< glottal source
    float dcx, dcy;             ///< DC blocker
    float av, ah, af;           ///< levels now (ramped through each frame)
    float dav, dah, daf;        ///< their step per sample in this frame
    uint32_t rng;               ///< noise
};

/// Sets a resonator to frequency f and bandwidth bw (unity gain at DC).
static void res_set(res_t *r, float f, float bw) {
    float e = expf(-(float)M_PI * bw / FS);
    r->c = -e * e;
    r->b = 2.0f * e * cosf(2.0f * (float)M_PI * f / FS);
    r->a = 1.0f - r->b - r->c;
}

/// Runs a sample through a resonator.
static inline float res_run(res_t *r, float x) {
    float y = r->a * x + r->b * r->y1 + r->c * r->y2;
    r->y2 = r->y1;
    r->y1 = y;
    return y;
}

/// White noise, -1 to 1.
static inline float noise(tts_t *t) {
    uint32_t x = t->rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    t->rng = x;
    return (float)(int32_t)x * (1.0f / 2147483648.0f);
}

/// A new segment at the end of the list.
/// @param t the state
/// @param ms its length
/// @param speed the speed (1: normal)
/// @return it, filled from nothing (silence)
static seg_t *seg_add(tts_t *t, float ms, float speed) {
    seg_t *s = &t->seg[t->nseg++];
    memset(s, 0, sizeof *s);
    s->start = t->total;
    s->len = (uint32_t)(ms * FS / 1000.0f / speed);
    if (s->len < 16)
        s->len = 16;
    t->total += s->len;
    s->tr = (uint16_t)(0.025f * FS);
    s->f[0] = 500, s->f[1] = 1500, s->f[2] = 2500;
    s->b[0] = 90, s->b[1] = 110, s->b[2] = 170;
    s->fc = 4000, s->fb = 3000;
    s->f0 = 1;
    return s;
}

/// Sets a segment's formants.
static void seg_formants(seg_t *s, const uint16_t *f) {
    for (int i = 0; i < 3; ++i)
        s->f[i] = f[i];
}

/// One phoneme token from the phoneme string.
typedef struct {
    int16_t ph;     ///< the phoneme, or -1 for a mark
    char mark;      ///< '/' ',' '.' '?' (for a mark)
    uint8_t stress; ///< stressed
} item_t;

tts_t *tts_start(const char *phonemes, const tts_voice_t *voice, const char **bad) {
    if (bad)
        *bad = NULL;
    // The tokens.
    int n = 0;
    for (const char *s = phonemes; *s; ++s)
        n += !isspace((unsigned char)*s) && (s == phonemes || isspace((unsigned char)s[-1]));
    item_t *it = malloc((n + 1) * sizeof *it);
    tts_t *t = calloc(1, sizeof *t);
    if (!it || !t) {
        free(it);
        free(t);
        return NULL;
    }
    int ni = 0;
    for (const char *s = phonemes; *s;) {
        while (isspace((unsigned char)*s))
            ++s;
        if (!*s)
            break;
        const char *e = s;
        while (*e && !isspace((unsigned char)*e))
            ++e;
        item_t *x = &it[ni];
        x->ph = -1;
        x->mark = 0;
        x->stress = 0;
        if (e - s == 1 && strchr("/,.?", *s))
            x->mark = *s;
        else {
            int len = (int)(e - s);
            if (len > 1 && (e[-1] == '1' || e[-1] == '2' || e[-1] == '0')) {
                x->stress = e[-1] != '0';
                --len;
            }
            x->ph = (int16_t)ph_find(s, len);
            if (x->ph < 0 && bad && !*bad)
                *bad = s;
        }
        if (x->ph >= 0 || x->mark)
            ++ni;
        s = e;
    }

    t->seg = malloc((size_t)(3 * ni + 4) * sizeof *t->seg);
    if (!t->seg) {
        free(it);
        free(t);
        return NULL;
    }
    float speed = voice && voice->speed ? voice->speed / 100.0f : 1.0f;
    if (speed < 0.4f)
        speed = 0.4f;
    if (speed > 2.5f)
        speed = 2.5f;
    t->base = voice && voice->pitch ? (float)voice->pitch : 110.0f;
    if (t->base < 50)
        t->base = 50;
    if (t->base > 300)
        t->base = 300;

    seg_add(t, 40, 1);  // a moment of quiet first
    int phrase0 = t->nseg;
    for (int i = 0; i < ni; ++i) {
        item_t *x = &it[i];
        if (x->ph < 0) {
            if (x->mark == '/')
                continue;
            // A pause ends the phrase: the pitch falls through it, and its
            // last vowel falls further, rises (?) or lifts a little (,).
            float fin = x->mark == '?' ? 1.35f : x->mark == ',' ? 1.06f : 0.82f;
            uint32_t a = t->seg[phrase0].start, len = t->total - a;
            int last = -1;
            for (int k = phrase0; k < t->nseg; ++k) {
                seg_t *s = &t->seg[k];
                float m = (float)(s->start + s->len / 2 - a) / (float)(len ? len : 1);
                s->f0 *= 1.1f - 0.2f * m;
                if (s->vowel)
                    last = k;
            }
            if (last >= 0)
                for (int k = last; k < t->nseg && t->seg[k].vowel; ++k)
                    t->seg[k].f0 *= fin;
            seg_add(t, x->mark == ',' ? 160 : 320, speed);
            phrase0 = t->nseg;
            continue;
        }
        const ph_t *p = &phons[x->ph];
        // The neighbours, across word marks but not pauses.
        const item_t *next = NULL, *prev = NULL;
        bool word_end = !(i + 1 < ni && it[i + 1].ph >= 0);
        for (int k = i + 1; k < ni; ++k) {
            if (it[k].ph >= 0) {
                next = &it[k];
                break;
            }
            if (it[k].mark != '/')
                break;
        }
        for (int k = i - 1; k >= 0; --k) {
            if (it[k].ph >= 0) {
                prev = &it[k];
                break;
            }
            if (it[k].mark != '/')
                break;
        }
        const ph_t *np = next ? &phons[next->ph] : NULL;
        bool next_sound = np && np->kind <= K_SONOR;  // a vowel or a glide follows
        bool phrase_end = true;  // no more vowels before the pause
        for (int k = i + 1; k < ni && (it[k].ph >= 0 || it[k].mark == '/'); ++k)
            if (it[k].ph >= 0 && phons[it[k].ph].kind <= K_DIPH)
                phrase_end = false;
        bool cluster = (np && np->kind >= K_NASAL) || (prev && phons[prev->ph].kind >= K_NASAL);

        float dur = p->dur;
        switch (p->kind) {
        case K_VOWEL:
        case K_DIPH: {
            if (!x->stress && strcmp(p->name, "AX"))  // (a schwa is short already)
                dur *= 0.72f;
            if (word_end)
                dur *= 1.2f;
            if (phrase_end)
                dur *= 1.35f;
            if (np && np->voiced && np->kind >= K_NASAL)
                dur *= 1.12f;
            float lift = x->stress ? 1.14f : 1.0f;
            if (p->kind == K_VOWEL) {
                seg_t *s = seg_add(t, dur, speed);
                seg_formants(s, p->f);
                s->b[0] = 70, s->b[1] = 100, s->b[2] = 150;
                s->av = x->stress ? 1.0f : 0.8f;
                s->tr = (uint16_t)(0.04f * FS);
                s->vowel = 1;
                s->stress = x->stress;
                s->f0 = lift;
            } else {  // a diphthong: the start, then a glide to the end
                seg_t *s = seg_add(t, dur * 0.5f, speed);
                seg_formants(s, p->f);
                s->b[0] = 70, s->b[1] = 100, s->b[2] = 150;
                s->av = x->stress ? 1.0f : 0.8f;
                s->tr = (uint16_t)(0.035f * FS);
                s->vowel = 1;
                s->stress = x->stress;
                s->f0 = lift;
                seg_t *g = seg_add(t, dur * 0.5f, speed);
                seg_formants(g, p->g);
                g->b[0] = 70, g->b[1] = 100, g->b[2] = 150;
                g->av = s->av * 0.9f;
                g->tr = (uint16_t)(g->len / 2 < 0.07f * FS ? g->len / 2 : 0.07f * FS);
                g->vowel = 1;
                g->stress = x->stress;
                g->f0 = lift * 0.97f;
            }
            break;
        }
        case K_SONOR:
        case K_NASAL: {
            if (cluster)
                dur *= 0.85f;
            if (word_end && !next)
                dur *= 1.3f;
            seg_t *s = seg_add(t, dur, speed);
            seg_formants(s, p->f);
            s->av = p->av / 100.0f;
            s->tr = (uint16_t)((p->kind == K_NASAL ? 0.015f : 0.035f) * FS);
            if (p->kind == K_NASAL)
                s->b[0] = 100, s->b[1] = 300, s->b[2] = 300;
            if (!strcmp(p->name, "L") && (!np || np->kind > K_SONOR)) {
                s->f[1] = 850;  // a dark L, at the end of a syllable ("people")
                s->f[2] = 2600;
            }
            break;
        }
        case K_FRIC: {
            if (cluster)
                dur *= 0.85f;
            if (word_end && !next)
                dur *= 1.25f;
            seg_t *s = seg_add(t, dur, speed);
            seg_formants(s, p->f);
            s->av = p->av / 100.0f;
            s->af = p->af / 100.0f;
            s->fc = p->fc, s->fb = p->fb;
            s->tr = (uint16_t)(0.025f * FS);
            break;
        }
        case K_ASP: {  // the next sound's formants, breathed
            seg_t *s = seg_add(t, dur, speed);
            s->ah = 0.55f;
            s->tr = 16;
            if (np && np->kind <= K_SONOR)
                seg_formants(s, np->f);
            break;
        }
        case K_STOP:
        case K_AFFR: {
            // The closure, then the release: a burst (and aspiration if
            // unvoiced and a vowel follows, but not after S); an affricate
            // releases into its fricative instead.
            const ph_t *st = p->kind == K_AFFR ? &phons[ph_find(p->voiced ? "D" : "T", 1)] : p;
            if (cluster)
                dur *= 0.8f;
            seg_t *c = seg_add(t, dur, speed);
            seg_formants(c, st->f);
            c->tr = (uint16_t)(0.02f * FS);
            if (st->voiced) {  // a voice bar: a low hum through the closure
                c->av = 0.2f;
                c->f[0] = 180;
                c->b[0] = 120;
            }
            seg_t *r = seg_add(t, st->voiced ? 7 : 11, speed);
            seg_formants(r, st->f);
            r->tr = 16;
            r->af = st->af / 100.0f * (word_end && !next ? 0.6f : 1.0f);
            r->ah = 0.15f;
            r->fc = st->fc, r->fb = st->fb;
            if (st->voiced)
                r->av = 0.35f;
            if (p->kind == K_AFFR) {
                const ph_t *f = &phons[ph_find(p->voiced ? "ZH" : "SH", 2)];
                seg_t *s = seg_add(t, p->voiced ? 55 : 75, speed);
                seg_formants(s, f->f);
                s->av = f->av / 100.0f;
                s->af = f->af / 100.0f;
                s->fc = f->fc, s->fb = f->fb;
            } else if (!st->voiced && next_sound &&
                       !(prev && !strcmp(phons[prev->ph].name, "S") && !word_end)) {
                seg_t *a = seg_add(t, next->stress ? 45 : 25, speed);
                seg_formants(a, np->f);
                a->ah = 0.45f;
                a->af = 0.06f;
                a->fc = st->fc, a->fb = st->fb;
                a->tr = (uint16_t)(0.03f * FS);
            }
            break;
        }
        }
    }
    // The pitch through the last phrase, if the phonemes didn't end with a pause.
    if (phrase0 < t->nseg) {
        uint32_t a = t->seg[phrase0].start, len = t->total - a;
        for (int k = phrase0; k < t->nseg; ++k) {
            seg_t *s = &t->seg[k];
            s->f0 *= 1.1f - 0.2f * (float)(s->start + s->len / 2 - a) / (float)(len ? len : 1);
        }
    }
    seg_add(t, 60, 1);  // and some quiet after
    free(it);

    t->rng = 0x12345678;
    t->f0 = t->base;
    for (int i = 0; i < 3; ++i)
        res_set(&t->r[i], 500 + 1000 * i, 100);
    res_set(&t->r[3], 3500, 250);
    res_set(&t->r[4], 4500, 300);
    return t;
}

/// Blends one group of a segment's parameters with its neighbour's near its
/// ends, over `tr` samples on each side (at most half of each segment).
/// @param t the state
/// @param i the segment sample s is in
/// @param s the sample
/// @param formants use each segment's own formant glide length (else `fixed`)
/// @param fixed the glide length in samples if not `formants`
/// @param j set to the neighbour blended with (or i)
/// @return the neighbour's weight
static float blend(const tts_t *t, int i, uint32_t s, bool formants, uint32_t fixed, int *j) {
    const seg_t *g = &t->seg[i];
#define HALF(k) (formants ? (t->seg[k].tr < t->seg[k].len / 2 ? t->seg[k].tr : t->seg[k].len / 2) \
                          : (fixed < t->seg[k].len / 2 ? fixed : t->seg[k].len / 2))
    uint32_t h = HALF(i);
    *j = i;
    if (i > 0 && s < g->start + h) {
        uint32_t hl = HALF(i - 1);
        *j = i - 1;
        return 1.0f - (float)(s + hl - g->start) / (float)(hl + h);
    }
    if (i + 1 < t->nseg && s >= g->start + g->len - h) {
        uint32_t hr = HALF(i + 1);
        *j = i + 1;
        return (float)(s - (g->start + g->len - h)) / (float)(h + hr);
    }
#undef HALF
    return 0;
}

/// The parameters at sample s.
/// @param t the state
/// @param s the sample (before the end)
/// @param i the segment it is in
/// @param p filled in
static void params(const tts_t *t, uint32_t s, int i, par_t *p) {
    const seg_t *a = &t->seg[i], *b;
    int j;
    float w = blend(t, i, s, true, 0, &j);
    b = &t->seg[j];
    for (int k = 0; k < 3; ++k) {
        p->f[k] = a->f[k] + (b->f[k] - a->f[k]) * w;
        p->b[k] = a->b[k] + (b->b[k] - a->b[k]) * w;
    }
    w = blend(t, i, s, false, AMP_TR * TTS_RATE / 1000, &j);
    b = &t->seg[j];
    p->av = a->av + (b->av - a->av) * w;
    p->ah = a->ah + (b->ah - a->ah) * w;
    p->af = a->af + (b->af - a->af) * w;
    p->fc = a->fc + (b->fc - a->fc) * w;
    p->fb = a->fb + (b->fb - a->fb) * w;
    w = blend(t, i, s, false, PITCH_TR * TTS_RATE / 1000, &j);
    b = &t->seg[j];
    p->f0 = a->f0 + (b->f0 - a->f0) * w;
}

/// The segment sample s is in, looking on from segment i.
static int seg_of(const tts_t *t, int i, uint32_t s) {
    while (i + 1 < t->nseg && s >= t->seg[i].start + t->seg[i].len)
        ++i;
    return i;
}

/// Sets up the next frame: the filters from the parameters at its middle,
/// and the levels ramping to those at its end.
static void frame(tts_t *t) {
    par_t p, e;
    uint32_t mid = t->pos + FRAME / 2, end = t->pos + FRAME;
    if (mid >= t->total)
        mid = t->total - 1;
    if (end >= t->total)
        end = t->total - 1;
    params(t, mid, seg_of(t, t->cur, mid), &p);
    params(t, end, seg_of(t, t->cur, end), &e);
    for (int k = 0; k < 3; ++k)
        res_set(&t->r[k], p.f[k], p.b[k]);
    t->f0 = t->base * p.f0;
    float w = 2.0f * (float)M_PI * p.fc / FS, q = p.fc / (p.fb > 50 ? p.fb : 50);
    float al = sinf(w) / (2.0f * q), a0 = 1.0f + al;
    t->fb0 = al / a0;
    t->fa1 = -2.0f * cosf(w) / a0;
    t->fa2 = (1.0f - al) / a0;
    t->dav = (e.av - t->av) / FRAME;
    t->dah = (e.ah - t->ah) / FRAME;
    t->daf = (e.af - t->af) / FRAME;
}

size_t tts_render(tts_t *t, int16_t *out, size_t n) {
    size_t k = 0;
    for (; k < n && t->pos < t->total; ++k, ++t->pos) {
        if (t->pos % FRAME == 0) {
            t->cur = seg_of(t, t->cur, t->pos);
            frame(t);
        }
        t->av += t->dav;
        t->ah += t->dah;
        t->af += t->daf;
        // The glottal pulse: the flow's slope, ending in a sharp closure.
        t->phase += t->f0 / FS;
        if (t->phase >= 1.0f) {
            t->phase -= 1.0f;
            t->f0 *= 1.0f + 0.01f * noise(t);  // a little jitter
        }
        float g = 0;
        if (t->phase < OPEN) {
            float x = t->phase / OPEN;
            g = x * (2.0f - 3.0f * x);
        }
        // A touch brighter: the slope less what came before it, in part.
        float gv = g - 0.4f * t->tilt;
        t->tilt = g;
        float nz = noise(t);
        float x = gv * t->av * G_VOICE * 100.0f + nz * t->ah * G_ASP;
        for (int r = 0; r < 5; ++r)
            x = res_run(&t->r[r], x);
        // Frication: noise through a band-pass beside the cascade.
        float fn = nz * t->af;
        float fy = t->fb0 * (fn - t->fx2) - t->fa1 * t->fy1 - t->fa2 * t->fy2;
        t->fx2 = t->fx1;
        t->fx1 = fn;
        t->fy2 = t->fy1;
        t->fy1 = fy;
        x += fy * G_FRIC;
        // No DC; and a soft limit near full scale.
        float y = x - t->dcx + 0.995f * t->dcy;
        t->dcx = x;
        t->dcy = y;
        if (y > 0.8f)
            y = 0.8f + 0.2f * tanhf((y - 0.8f) * 5.0f);
        else if (y < -0.8f)
            y = -0.8f + 0.2f * tanhf((y + 0.8f) * 5.0f);
        out[k] = (int16_t)(y * 32000.0f);
    }
    return k;
}

size_t tts_length(const tts_t *t) {
    return t->total;
}

void tts_end(tts_t *t) {
    if (t) {
        free(t->seg);
        free(t);
    }
}
