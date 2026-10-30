/**
 * @file htmltest.c
 * @brief The web browser's page engine on a PC: URL resolving, form data,
 *        and pages of random and cut-off HTML laid out under ASan (every
 *        line must fit, every link run must be on its line).
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "www_html.h"

bool pc_psram_full;  ///< PSRAM has run out

/// www_toggled_fn: every third menu opened by hand.
/// @param id the menu
/// @return it was
static bool toggled(int id) { return id % 3 == 0; }
static int failures;  ///< checks that failed

/// Checks a URL resolves as expected.
/// @param base the page
/// @param rel the link
/// @param want the answer
static void resolve(const char *base, const char *rel, const char *want) {
    char out[WWW_URL_MAX];
    www_resolve(base, rel, out, sizeof out);
    if (strcmp(out, want)) {
        printf("FAIL: %s + %s = %s, not %s\n", base, rel, out, want);
        ++failures;
    }
}

/// Lays out a page and checks the page's arrays agree with each other.
/// @param src the HTML
/// @param len its length
/// @param html HTML rather than text
static void check_layout(const char *src, size_t len, bool html) {
    www_doc_t *d = www_layout(src, len, html, len & 1, len & 2, NULL, (len & 4) ? NULL : toggled);
    if (!d) {
        printf("FAIL: no page\n");
        ++failures;
        return;
    }
    for (int32_t i = 0; i < d->nlines; ++i)
        if (d->lines[i].len > WWW_COLS || d->lines[i].cell + d->lines[i].len > d->ncells) {
            printf("FAIL: line %d is %d long\n", i, d->lines[i].len);
            ++failures;
        }
    for (int32_t i = 0; i < d->nsegs; ++i) {
        const www_seg_t *s = &d->segs[i];
        if (s->line >= d->nlines || s->col + s->len > d->lines[s->line].len || s->link >= d->nlinks ||
            (i && s->line < d->segs[i - 1].line)) {
            printf("FAIL: link run %d is off its line\n", i);
            ++failures;
        }
    }
    for (int32_t i = 0; i < d->nlinks; ++i) {
        if (d->links[i].type == WL_SELECT)
            for (int k = 0; k < d->links[i].nopts; ++k)
                www_option(d, i, k, true);
        if (d->links[i].type != WL_HREF && d->links[i].type != WL_HIDDEN)
            www_set_value(d, i, "changed value");
    }
    char data[4096];
    for (int f = 0; f < d->nforms; ++f)
        www_form_data(d, f, -1, data, sizeof data);
    www_free(d);
}

int main(void) {
    resolve("http://a.com/b/c.html", "d.html", "http://a.com/b/d.html");
    resolve("http://a.com/b/c.html", "../d.html", "http://a.com/d.html");
    resolve("http://a.com/b/c.html", "/x/./y/../z", "http://a.com/x/z");
    resolve("http://a.com/b/c.html?q=1#f", "?q=2", "http://a.com/b/c.html?q=2");
    resolve("http://a.com/b/c.html#f", "#g", "http://a.com/b/c.html#g");
    resolve("https://a.com/b/", "//cdn.org/x", "https://cdn.org/x");
    resolve("https://a.com", "x", "https://a.com/x");
    resolve("https://a.com", "?q", "https://a.com/?q");
    resolve("http://a.com/b/c", "HTTPS://Other.org/../p", "HTTPS://Other.org/p");
    resolve("http://a.com/b/c", "mailto:x@y.z", "mailto:x@y.z");
    resolve("http://a.com/b/c", "  e f \n", "http://a.com/b/e f");
    resolve("http://a.com/b/c", "", "http://a.com/b/c");
    resolve("file:///WWW/HOME.HTM", "PAGES/A.HTM", "file:///WWW/PAGES/A.HTM");
    resolve("file:///WWW/HOME.HTM", "../X.HTM", "file:///X.HTM");
    char enc[64];
    www_url_encode("a b&c=\x82", enc, sizeof enc);  // CP437 0x82 is é
    if (strcmp(enc, "a+b%26c%3D%C3%A9")) {
        printf("FAIL: encoded as %s\n", enc);
        ++failures;
    }
    // Pages of random tags, text and entities, and every cut-off of one page.
    static const char *const bits[] = {
        "<p>", "</p>", "<a href=x>", "</a>", "<ul>", "<li>", "</ul>", "<ol start=9>", "<pre>",
        "</pre>", "<table><tr><td>", "<td>", "</table>", "<h1>", "</h1>", "<br>", "<hr>",
        "<form action=/f>", "<input name=q size=99>", "<input type=checkbox name=c>",
        "<select name=s><option>a<option value=b selected>bb", "</select>", "<textarea name=t>",
        "</textarea>", "<button>", "</button>", "<img alt=\"pic\">", "&amp;", "&#x263a;", "&bogus",
        "\xc3\xa9", "\xe2\x80\x9c", "\xff", "word ", "longwordlongwordlongwordlongword", "  ",
        "\n", "\t", "<!-- c -->", "<!--", "<script>x</script>", "<style>", "<blockquote>",
        "</blockquote>", "<dl><dt>t<dd>", "<a name=n>", "<meta charset=latin1>", "<", "<x y='z>",
        "<a href=\"", "<iframe src=f>", "<nav aria-label=\"Site\">", "</nav>", "<details>",
        "<details open><summary>More <b>here</b></summary>", "</details>", "<summary>s", "<div class=\"vector-dropdown x\"><label>L <i>x</i></label>", "<div>", "</div>", "<li class=\"a more-dropdown\"><h4>T</h4>", "</li>", "<button type=button>B", "<meta http-equiv=refresh content=\"0;url=/r\">",
    };
    const int nbits = (int)(sizeof bits / sizeof *bits);
    static char page[65536];
    srand(1);
    for (int round = 0; round < 2000; ++round) {
        size_t n = 0;
        int parts = rand() % 400;
        for (int i = 0; i < parts; ++i) {
            const char *b = bits[rand() % nbits];
            size_t k = strlen(b);
            if (n + k >= sizeof page)
                break;
            memcpy(page + n, b, k);
            n += k;
        }
        check_layout(page, n, round % 7 != 0);
        if (round < 50)
            for (size_t cut = 0; cut < n; cut += 1 + n / 200)
                check_layout(page, cut, true);
    }
    // No PSRAM at all: no page, and no crash.
    pc_psram_full = true;
    if (www_layout("<p>x", 4, true, false, true, NULL, NULL)) {
        printf("FAIL: a page with no memory\n");
        ++failures;
    }
    pc_psram_full = false;
    printf("htmltest: %s\n", failures ? "FAILED" : "all checks passed");
    return failures != 0;
}
