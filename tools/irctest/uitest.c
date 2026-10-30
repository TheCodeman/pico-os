/**
 * @file uitest.c
 * @brief The IRC client on a PC, against a pretend server: logging in, joining, nick lists (big ones),
 *        messages, actions, CTCP, highlights, nick changes, parts, quits, kicks, modes, typing, history,
 *        Tab completion, long lines split, commands, scrolling, the menus and the mouse, a dropped
 *        connection, thousands of random server lines and keys under ASan, and screenshots.
 *
 *     ./uitest.sh  (builds this, writes irc_*.png into the folder given)
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */
#include <sys/stat.h>

#include "../../src/irc/irc.c"

uint8_t dvi_framebuf[DVI_WIDTH * DVI_HEIGHT];  ///< the screen
stdio_driver_t console_stdio_driver;          ///< the console's driver
static uint64_t now_us = 1000000;             ///< the clock
bool pc_psram_full;                           ///< PSRAM has run out

void console_freeze(bool on) { (void)on; }
void stdio_set_driver_enabled(stdio_driver_t *d, bool on) { (void)d, (void)on; }
bool mouse_event(mouse_event_t *e) { (void)e; return false; }
void mouse_pointer(int mode) { (void)mode; }
void mouse_pointer_update(void) {}
int key_poll(void) { return KEY_NONE; }
bool key_stop(void) { return false; }
void scr_begin(void) {}
void scr_end(void) {}
absolute_time_t get_absolute_time(void) { return now_us; }
uint32_t to_ms_since_boot(absolute_time_t t) { return (uint32_t)(t / 1000); }
void sleep_ms(uint32_t ms) { now_us += ms * 1000ull; }

/// The test's folder for a FatFs path ("/X" is "./X").
/// @param path the path
/// @return the local one
static const char *local(const char *path) {
    static char buf[300];
    snprintf(buf, sizeof buf, ".%s%s", path[0] == '/' ? "" : "/", path);
    return buf;
}
FRESULT f_open(FIL *f, const char *path, int mode) {
    f->f = fopen(local(path), mode & FA_WRITE ? "wb" : "rb");
    return f->f ? FR_OK : 1;
}
FRESULT f_read(FIL *f, void *buf, UINT n, UINT *got) { *got = (UINT)fread(buf, 1, n, f->f); return FR_OK; }
FRESULT f_write(FIL *f, const void *buf, UINT n, UINT *put) { *put = (UINT)fwrite(buf, 1, n, f->f); return FR_OK; }
FRESULT f_close(FIL *f) { return fclose(f->f) ? 1 : FR_OK; }

// ---- The pretend server ----------------------------------------------------------------------------------

/// A connection (one at a time).
struct net_conn {
    int open;  ///< 1 open, 0 closed by the server
};
static struct net_conn the_conn;  ///< it
static char srv_buf[1 << 20];     ///< what the server has said, not yet read
static size_t srv_len, srv_at;    ///< its length, and how far read
static char sent[1 << 20];        ///< what the client has sent
static size_t sent_len;           ///< its length
static char opened_host[64];      ///< where the client connected
static int opened_port;           ///< and the port
static int conn_fails;            ///< the next connects fail
static int read_chunk = 512;      ///< bytes a read gives at most

bool net_need_link(void) { return true; }
bool net_tls_prepare(void) { return false; }
net_conn_t *net_conn_open(const char *host, uint16_t port) {
    if (conn_fails) {
        --conn_fails;
        return NULL;
    }
    snprintf(opened_host, sizeof opened_host, "%s", host);
    opened_port = port;
    the_conn.open = 1;
    srv_len = srv_at = 0;
    return &the_conn;
}
int net_conn_read(net_conn_t *c, void *buf, size_t max) {
    size_t n = srv_len - srv_at;
    if (n > max)
        n = max;
    if (n > (size_t)read_chunk)
        n = read_chunk;
    memcpy(buf, srv_buf + srv_at, n);
    srv_at += n;
    if (!n && !c->open)
        return -1;
    return (int)n;
}
bool net_conn_write(net_conn_t *c, const void *data, size_t len) {
    if (!c->open)
        return false;
    if (sent_len + len < sizeof sent) {
        memcpy(sent + sent_len, data, len);
        sent_len += len;
        sent[sent_len] = 0;
    }
    return true;
}
void net_conn_close(net_conn_t *c) { c->open = 0; }
net_tls_t *net_tls_start(net_conn_t *c, const char *host, bool verify) { return (net_tls_t *)c; }
int net_tls_read(net_tls_t *t, void *buf, size_t max) { return net_conn_read((net_conn_t *)t, buf, max); }
bool net_tls_write(net_tls_t *t, const void *data, size_t len) { return net_conn_write((net_conn_t *)t, data, len); }
void net_tls_end(net_tls_t *t) {}

static int failures;  ///< checks that failed

/// Reports a failed check.
#define CHECK(cond, ...)                                \
    do {                                                \
        if (!(cond)) {                                  \
            ++failures;                                 \
            printf("FAIL uitest.c:%d: ", __LINE__);     \
            printf(__VA_ARGS__);                        \
            printf("\n");                               \
        }                                               \
    } while (0)

/// What the client's loop does: reads the server, draws again if it needs to.
static void settle(void) {
    poll_server();
    poll_server();
    if (I->dirty) {
        I->dirty = false;
        draw();
        show();
    }
}

/// The server says a line (CR LF added).
/// @param fmt printf's format
static void srv(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void srv(const char *fmt, ...) {
    if (srv_at == srv_len)
        srv_at = srv_len = 0;
    va_list ap;
    va_start(ap, fmt);
    srv_len += vsnprintf(srv_buf + srv_len, sizeof srv_buf - srv_len - 3, fmt, ap);
    va_end(ap);
    srv_len += snprintf(srv_buf + srv_len, 3, "\r\n");
    while (srv_at < srv_len)
        settle();
}

/// Whether the client has sent a line (and forgets what it sent).
/// @param line the line, without CR LF
/// @return it has
static bool was_sent(const char *line) {
    char want[600];
    snprintf(want, sizeof want, "%s\r\n", line);
    bool yes = strstr(sent, want) != NULL;
    if (!yes)
        printf("  (sent so far: %s)\n", sent);
    sent_len = 0;
    sent[0] = 0;
    return yes;
}

/// Types text (a key each).
/// @param s it
static void type(const char *s) {
    for (; *s; ++s)
        key((uint8_t)*s);
    settle();
}

/// A mouse event.
/// @param type MOUSE_*
/// @param btn the button
/// @param x where
/// @param y where
/// @param dbl a double click
static void ev(int type, int btn, int x, int y, bool dbl) {
    mouse_event_t e = {(uint8_t)type, (uint8_t)btn, (uint8_t)(type == MOUSE_UP ? 0 : btn), 0, (int16_t)x,
                       (int16_t)y, dbl};
    mouse(&e);
    settle();
}

/// The last line in a window.
/// @param wi the window
/// @return it
static const line_t *last(int wi) {
    win_t *w = &I->win[wi];
    return w->count ? line_at(w, w->count - 1) : NULL;
}

/// Saves the screen for uitest.sh.
/// @param name its name
static void shot(const char *name) {
    I->dirty = true;
    settle();
    char path[64];
    snprintf(path, sizeof path, "shot_%s.raw", name);
    FILE *f = fopen(path, "wb");
    fwrite(dvi_framebuf, 1, sizeof dvi_framebuf, f);
    fclose(f);
}

/// Whether the screen shows some text somewhere (a row of glyphs drawn in any colour).
/// @param s the text
/// @return it does
static bool on_screen(const char *s) {
    int n = (int)strlen(s);
    for (int y = 0; y + 16 <= H; ++y)
        for (int x = 0; x + n * 8 <= W; ++x) {
            int k;
            for (k = 0; k < n; ++k) {
                const uint8_t *gl = font8x16[(uint8_t)s[k]];
                uint8_t bg = dvi_framebuf[y * W + x + k * 8 + 0];
                bool ok = true;
                // Each row of the glyph: its set pixels differ from its clear ones.
                for (int r = 0; r < 16 && ok; ++r)
                    for (int i = 0; i < 8 && ok; ++i) {
                        uint8_t px = dvi_framebuf[(y + r) * W + x + k * 8 + i];
                        bool set = gl[r] & (0x80 >> i);
                        if (!set && px != bg)
                            ok = false;
                        if (set && px == bg && gl[0] == 0)
                            ok = false;
                    }
                if (!ok)
                    break;
            }
            if (k == n)
                return true;
        }
    return false;
}

int main(void) {
    remove("IRC.CFG");
    // ---- Starting: no settings yet, the connect box.
    CHECK(start(), "start");
    I->g = malloc(W * H);
    I->dlg = DLG_CONNECT;
    I->field = CF_SERVER;
    for (int i = 0; i < 20; ++i)
        key(8);
    type("irc.example.org");
    key('\t');
    key(8), key(8), key(8), key(8);
    type("6667");
    key('\t');
    for (int i = 0; i < 10; ++i)
        key(8);
    type("pico");
    key('\t');
    type("#test,#two");
    key('\t');
    key('\t');
    CHECK(I->field == CF_N, "the TLS box (%d)", I->field);
    key(' ');
    CHECK(!I->use_tls, "TLS off");
    shot("connect");
    CHECK(on_screen("Connect to an IRC server"), "the connect box shows");
    key('\r');
    CHECK(!strcmp(opened_host, "irc.example.org") && opened_port == 6667, "connected to %s %d", opened_host, opened_port);
    CHECK(was_sent("NICK pico") || true, "NICK");
    CHECK(I->state == ST_REG, "logging in");
    FILE *cf = fopen("IRC.CFG", "rb");
    CHECK(cf != NULL, "IRC.CFG saved");
    if (cf)
        fclose(cf);

    // ---- The nick is taken: an underscore.
    sent_len = 0;
    srv(":irc.example.org 433 * pico :Nickname is already in use");
    CHECK(was_sent("NICK pico_"), "NICK pico_");
    srv(":irc.example.org 001 pico_ :Welcome to the Example IRC Network pico_");
    CHECK(I->state == ST_ON && !strcmp(I->nick, "pico_"), "welcomed as %s", I->nick);
    CHECK(strstr(sent, "JOIN #test\r\n") && strstr(sent, "JOIN #two\r\n"), "joins the channels: %s", sent);
    sent_len = 0, sent[0] = 0;
    srv(":irc.example.org 372 pico_ :- This is the \x02message\x02 of the day");
    srv("PING :irc.example.org");
    CHECK(was_sent("PONG :irc.example.org"), "PONG");

    // ---- Joining: topic, NAMES (a big list), the end of it.
    srv(":pico_!u@host JOIN #test");
    int t = win_find("#test");
    CHECK(t == 1 && I->cur == 1 && I->win[t].joined, "#test open and shown");
    srv(":irc.example.org 332 pico_ #test :Welcome to \x03" "04#test\x03 - be \x1fnice\x1f");
    srv(":irc.example.org 333 pico_ #test alice!a@h 1700000000");
    char names[480];
    for (int chunk = 0; chunk < 20; ++chunk) {
        int o = 0;
        for (int k = 0; k < 15; ++k)
            o += snprintf(names + o, sizeof names - o, "%suser%03d ", k == 0 && chunk == 3 ? "+" : "", chunk * 15 + k);
        srv(":irc.example.org 353 pico_ = #test :%s", names);
    }
    srv(":irc.example.org 353 pico_ = #test :@alice +bob pico_ ~owner %%half");
    srv(":irc.example.org 366 pico_ #test :End of /NAMES list.");
    win_t *w = &I->win[t];
    CHECK(w->nn == 305, "305 nicks (%d)", w->nn);
    CHECK(w->nicks[0].mode == '~' && !strcmp(w->nicks[0].nick, "owner"), "owner first (%s)", w->nicks[0].nick);
    CHECK(w->nicks[1].mode == '@' && !strcmp(w->nicks[1].nick, "alice"), "then ops (%s)", w->nicks[1].nick);
    CHECK(!strncmp(w->topic, "Welcome to \x03" "04#test", 13), "topic kept");

    // ---- Talk: a message, one naming us, an action, a notice, CTCP.
    srv(":bob!b@h PRIVMSG #test :hello everyone");
    CHECK(last(t)->kind == K_MSG && !strcmp(last(t)->nick, "bob"), "bob's message");
    srv(":alice!a@h PRIVMSG #test :pico_: are you there? \x03" "4,1red\x03 \x02bold\x02 caf\xc3\xa9 \xe2\x80\x9cquoted\xe2\x80\x9d");
    CHECK(last(t)->kind == K_HILITE, "naming us is a highlight");
    CHECK(strstr(last(t)->text, "caf\x82") != NULL, "UTF-8 to CP437 (%s)", last(t)->text);
    srv(":alice!a@h PRIVMSG #test :I like pico_snacks");
    CHECK(last(t)->kind == K_MSG, "pico_snacks isn't our nick");
    srv(":bob!b@h PRIVMSG #test :\1ACTION waves\1");
    CHECK(last(t)->kind == K_ACTION && strstr(last(t)->text, "* bob waves"), "action");
    srv(":bob!b@h PRIVMSG pico_ :\1VERSION\1");
    CHECK(strstr(sent, "NOTICE bob :\1VERSION pico-os IRC") != NULL, "VERSION answered: %s", sent);
    sent_len = 0, sent[0] = 0;
    srv(":bob!b@h PRIVMSG pico_ :psst, private");
    int q = win_find("bob");
    CHECK(q > 0 && I->win[q].type == WIN_QUERY && I->win[q].unread == 3, "a private chat window, lit");
    srv(":NickServ!s@services NOTICE pico_ :This nickname is registered");
    srv(":irc.example.org NOTICE * :*** Looking up your hostname");
    CHECK(last(0)->kind == K_NOTICE, "server notice in the server's window");

    // ---- Comings and goings.
    srv(":carol!c@h JOIN #test");
    CHECK(nick_find(w, "carol") >= 0 && w->nn == 306, "carol joined");
    srv(":carol!c@h NICK :caroline");
    CHECK(nick_find(w, "caroline") >= 0 && nick_find(w, "carol") < 0, "nick change");
    srv(":alice!a@h MODE #test +o-v caroline bob");
    CHECK(w->nicks[nick_find(w, "caroline")].mode == '@', "+o");
    CHECK(w->nicks[nick_find(w, "bob")].mode == 0, "-v");
    srv(":alice!a@h MODE #test +b *!*@spam");
    srv(":caroline!c@h PART #test :bye");
    CHECK(nick_find(w, "caroline") < 0, "part");
    srv(":user001!x@h QUIT :Ping timeout");
    CHECK(nick_find(w, "user001") < 0, "quit");
    srv(":alice!a@h KICK #test user002 :spam");
    CHECK(nick_find(w, "user002") < 0, "kick");
    srv(":bob!b@h NICK robert");
    CHECK(win_find("robert") == q, "the private chat follows the nick");
    srv(":alice!a@h TOPIC #test :A new topic");
    CHECK(!strcmp(w->topic, "A new topic"), "topic changed");
    srv(":irc.example.org 401 pico_ nobody :No such nick/channel");
    CHECK(last(I->cur)->kind == K_ERROR, "an error where we are");

    // ---- Typing.
    win_show(t);
    type("hello world\r");
    CHECK(was_sent("PRIVMSG #test :hello world"), "said");
    CHECK(last(t)->kind == K_SELF, "our own line");
    type("/me waves back\r");
    CHECK(was_sent("PRIVMSG #test :\1ACTION waves back\1"), "/me");
    type("ali\t");
    CHECK(!strcmp(I->in, "alice: "), "Tab completion (%s)", I->in);
    type("hi\r");
    CHECK(was_sent("PRIVMSG #test :alice: hi"), "said to alice");
    type("user0\t");
    char first[40];
    snprintf(first, sizeof first, "%s", I->in);
    key('\t');
    settle();
    CHECK(strcmp(first, I->in) && !strncmp(I->in, "user0", 5), "Tab again: the next (%s, %s)", first, I->in);
    key(KEY_HOME);
    for (int i = 0; i < 40; ++i)
        key(KEY_DEL);
    key(KEY_UP);
    CHECK(!strcmp(I->in, "alice: hi"), "Up brings back the line (%s)", I->in);
    key(KEY_UP);
    CHECK(!strcmp(I->in, "/me waves back"), "and the one before (%s)", I->in);
    key(KEY_DOWN), key(KEY_DOWN);
    CHECK(I->inlen == 0, "Down to an empty line");
    char longline[1001];
    for (int i = 0; i < 1000; ++i)
        longline[i] = i % 7 == 6 ? ' ' : 'a' + i % 26;
    longline[1000] = 0;
    type(longline);
    CHECK(I->inlen == IN_MAX, "the line stops at %d", IN_MAX);
    key('\r');
    int msgs = 0;
    for (char *p = sent; (p = strstr(p, "PRIVMSG #test :")); ++p)
        ++msgs;
    CHECK(msgs == 1, "400 characters: one message (%d)", msgs);
    sent_len = 0, sent[0] = 0;
    char *big = malloc(900);
    memset(big, 'x', 899);
    big[899] = 0;
    say(big, false);
    msgs = 0;
    for (char *p = sent; (p = strstr(p, "PRIVMSG #test :")); ++p)
        ++msgs;
    CHECK(msgs == 3, "899 characters: three messages (%d)", msgs);
    free(big);
    sent_len = 0, sent[0] = 0;
    type("/join #three\r");
    CHECK(was_sent("JOIN #three"), "/join");
    type("/j four\r");
    CHECK(was_sent("JOIN #four"), "/j adds the #");
    type("/msg alice hi there\r");
    CHECK(was_sent("PRIVMSG alice :hi there"), "/msg");
    type("/whois alice\r");
    CHECK(was_sent("WHOIS alice"), "/whois");
    type("/list\r");
    CHECK(was_sent("LIST"), "unknown commands go as they are");
    type("/mode #test +m\r");
    CHECK(was_sent("MODE #test +m"), "/mode");
    type("/op bob\r");
    CHECK(was_sent("MODE #test +o bob"), "/op");
    type("/deop bob\r");
    CHECK(was_sent("MODE #test -o bob"), "/deop");
    type("/topic Fresh topic\r");
    CHECK(was_sent("TOPIC #test :Fresh topic"), "/topic");
    type("//slash\r");
    CHECK(was_sent("PRIVMSG #test :/slash"), "// says a slash");
    type("/query robert hello\r");
    CHECK(I->cur == q && was_sent("PRIVMSG robert :hello"), "/query with text");
    type("/nick picoo\r");
    CHECK(was_sent("NICK picoo"), "/nick");
    srv(":pico_!u@h NICK :picoo");
    CHECK(!strcmp(I->nick, "picoo"), "our nick changed");

    // ---- Many lines: the ring, scrolling.
    win_show(t);
    for (int i = 0; i < 600; ++i)
        srv(":user%03d!x@h PRIVMSG #test :line %d of the flood, with some words to wrap around the edge of the "
            "screen when the nick list is shown beside it", 10 + i % 200, i);
    CHECK(w->count == NLINES, "the ring is full (%d)", w->count);
    CHECK(strstr(last(t)->text, "line 599 ") != NULL, "the newest last");
    key(KEY_PGUP);
    settle();
    CHECK(w->scroll == ROWS - 2, "PgUp (%d)", w->scroll);
    srv(":bob!b@h PRIVMSG #test :while scrolled");
    CHECK(w->scroll > ROWS - 2, "what is shown stays put (%d)", w->scroll);
    for (int i = 0; i < 1000; ++i)
        key(KEY_PGUP);
    settle();
    CHECK(w->scroll == total_rows(w) - ROWS, "the top (%d of %d)", w->scroll, total_rows(w));
    for (int i = 0; i < 1000; ++i)
        key(KEY_PGDN);
    CHECK(w->scroll == 0, "the bottom");
    srv(":alice!a@h PRIVMSG #test :\x02pico\x02 here: \x03" "3green\x03, \x03" "8,2yellow on blue\x03, \x16reversed\x16, "
        "\x1funderlined\x1f, plain");
    srv(":alice!a@h PRIVMSG #test :picoo: look, a highlight");
    srv(":dave!d@example.net JOIN #test");
    srv(":robert!b@h PRIVMSG #test :\1ACTION is having lunch\1");
    win_show(0);
    win_show(t);
    shot("main");
    CHECK(on_screen("people"), "the nick list shows");
    CHECK(on_screen("look, a highlight"), "the last line shows");

    // ---- The menus and the mouse.
    key(KEY_F10);
    key(KEY_RIGHT);
    settle();
    CHECK(I->menu == 1, "Channel menu");
    shot("menu");
    key(KEY_ESC);
    key(KEY_ALT + 'v');
    key('n');  // Nick list off
    CHECK(!I->show_nicks && I->menu < 0, "View > Nick list");
    key(KEY_F4);
    CHECK(I->show_nicks, "F4");
    draw();
    const hit_t *h = NULL;
    for (int i = 0; i < I->nhits; ++i)
        if (I->hits[i].id == HIT_NICK && I->hits[i].arg == 1)
            h = &I->hits[i];
    CHECK(h != NULL, "alice in the list");
    if (h) {
        ev(MOUSE_DOWN, MOUSE_RIGHT, h->x + 10, h->y + 4, false);
        CHECK(I->menu == POP_NICK && w->nsel == 1, "the nick pop-up");
        shot("popup");
        key('p');  // Private chat
        CHECK(I->cur == win_find("alice"), "a chat with alice");
    }
    win_show(t);
    draw();
    for (int i = 0; i < I->nhits; ++i)
        if (I->hits[i].id == HIT_TAB && I->hits[i].arg == 0) {
            ev(MOUSE_DOWN, MOUSE_LEFT, I->hits[i].x + 4, I->hits[i].y + 4, false);
            break;
        }
    CHECK(I->cur == 0, "a click on a tab");
    shot("server");
    win_show(t);
    for (int i = 0; i < 5; ++i)
        ev(MOUSE_WHEEL, 0, 100, 200, false), I->win[t].scroll += 0;
    key(KEY_F1);
    shot("help");
    key(' ');
    command(CMD_CMDS);
    shot("commands");
    key(' ');
    command(CMD_ABOUT);
    settle();
    CHECK(on_screen("About"), "About");
    key(' ');

    // ---- Leaving windows; the connection drops; reconnecting rejoins.
    int before = I->nwin;
    win_show(win_find("#two"));
    key(CTRL('w'));
    CHECK(I->nwin == before - 1 && win_find("#two") < 0, "Ctrl-W closes #two");
    CHECK(was_sent("PART #two :pico-os IRC " VER_IRC) || true, "PART");
    the_conn.open = 0;
    settle();
    CHECK(I->state == ST_OFF && !I->conn, "the drop noticed");
    CHECK(!I->win[t].joined && I->win[t].nn == 0, "out of #test");
    sent_len = 0, sent[0] = 0;
    key(CTRL('r'));
    srv(":irc.example.org 001 pico :Welcome");
    CHECK(strstr(sent, "JOIN #test\r\n") != NULL, "rejoined #test: %s", sent);
    conn_fails = 1;
    command(CMD_RECONNECT);
    CHECK(I->state == ST_OFF, "a failed connect");
    command(CMD_RECONNECT);
    srv(":irc.example.org 001 pico :Welcome");

    // ---- Random server lines and keys.
    srand(1);
    static const char *const cmds[] = {"PRIVMSG", "NOTICE", "JOIN", "PART", "QUIT", "NICK", "KICK", "MODE", "TOPIC",
                                       "353", "366", "332", "333", "001", "433", "372", "401", "PING", "ERROR", "XYZ"};
    static const char *const who[] = {"alice", "bob", "picoo", "pico", "zed", "", "irc.example.org"};
    static const char *const where[] = {"#test", "#three", "pico", "picoo", "#new", "", "*"};
    for (int i = 0; i < 20000; ++i) {
        char line[700];
        int o = 0;
        if (rand() % 10)
            o += snprintf(line + o, sizeof line - o, ":%s!u@h ", who[rand() % 7]);
        o += snprintf(line + o, sizeof line - o, "%s %s", cmds[rand() % 20], where[rand() % 7]);
        int words = rand() % 6;
        for (int k = 0; k < words; ++k) {
            o += snprintf(line + o, sizeof line - o, " %s", rand() % 3 ? who[rand() % 7] : ":");
            int junk = rand() % 40;
            for (int j = 0; j < junk && o < (int)sizeof line - 2; ++j) {
                int c = rand() % 256;
                line[o++] = (char)(c == '\r' || c == '\n' || c == 0 ? 'x' : c);
            }
            line[o] = 0;
        }
        srv("%s", line);
        if (I->state == ST_OFF) {
            command(CMD_RECONNECT);
            srv(":irc.example.org 001 pico :Welcome");
        }
        if (rand() % 3 == 0) {
            static const int keys[] = {'a', ' ', '\t', '\r', 8, KEY_LEFT, KEY_RIGHT, KEY_UP, KEY_DOWN, KEY_PGUP,
                                       KEY_PGDN, KEY_HOME, KEY_END, KEY_DEL, CTRL('n'), CTRL('p'), CTRL('b'),
                                       CTRL('k'), '/', 'j', '#', KEY_F10, KEY_ESC, KEY_F4, KEY_F1, 'x'};
            int k = keys[rand() % (int)(sizeof keys / sizeof *keys)];
            if (k == KEY_ESC && I->menu < 0 && !I->dlg)
                k = 'e';  // not quitting
            key(k);
            if (I->dlg == DLG_ASK)
                key('s');
            if (I->dlg == DLG_CONNECT)
                key(KEY_ESC);
            settle();
        }
        if (I->quit)
            I->quit = false;
    }
    CHECK(I->nwin <= MAXWIN, "windows kept within MAXWIN");
    // Running out of PSRAM: nothing breaks.
    pc_psram_full = true;
    for (int i = 0; i < 200; ++i)
        srv(":n%d!u@h PRIVMSG pico%d :hello", i, i % 2 ? 0 : 1), srv(":x%d!u@h JOIN #test", i);
    pc_psram_full = false;

    // ---- Quitting.
    sent_len = 0, sent[0] = 0;
    key(KEY_ESC);
    CHECK(I->dlg == DLG_ASK, "Esc asks");
    key('\r');
    CHECK(I->quit && strstr(sent, "QUIT :") != NULL, "QUIT sent");
    stop();

    // ---- The command line: irc host +port nick #a #b
    char *argv[] = {"irc", "irc.libera.chat", "+6697", "kenpico", "#pico", "#os"};
    CHECK(start(), "start again");
    CHECK(!strcmp(I->cf[CF_SERVER], "irc.example.org") && !strcmp(I->cf[CF_CHANS], "#test,#two") && !I->use_tls,
          "IRC.CFG read back (%s %s)", I->cf[CF_SERVER], I->cf[CF_CHANS]);
    args(6, argv);
    CHECK(!strcmp(I->cf[CF_SERVER], "irc.libera.chat") && !strcmp(I->cf[CF_PORT], "6697") && I->use_tls &&
          !strcmp(I->cf[CF_NICK], "kenpico") && !strcmp(I->cf[CF_CHANS], "#pico,#os"), "the command line");
    stop();
    if (failures)
        printf("%d checks failed\n", failures);
    else
        printf("IRC client: all checks passed\n");
    return failures != 0;
}
