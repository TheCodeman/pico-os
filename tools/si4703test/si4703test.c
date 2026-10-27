/**
 * @file si4703test.c
 * @brief PC tests of the Si4703 driver against a simulated chip.
 *
 *     cc -I../../src/fmradio si4703test.c sim4703.c ../../src/fmradio/si4703.c && ./a.out
 *
 *  - reset, power-up (the crystal first) and the registers the settings make;
 *  - the channel arithmetic of every band and spacing, both ways;
 *  - tuning and seeking, up and down, to the band's ends;
 *  - a scan of the band finds every station above the seek threshold;
 *  - RDS: the station name and radio text, with broken groups mixed in;
 *  - RDS block errors: uncorrectable blocks dropped, and a wrong PI, name or
 *    text from a miscorrected block never shown.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <string.h>

#include "sim4703.h"

static int failures;  ///< checks that failed

/// Reports a failed check.
#define CHECK(cond, ...)                                        \
    do {                                                        \
        if (!(cond)) {                                          \
            ++failures;                                         \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);         \
            printf(__VA_ARGS__);                                \
            printf("\n");                                       \
        }                                                       \
    } while (0)

/// The stations on the simulated air.
static const sim_station_t air[] = {
    {88100, 32, true, 0x1234, 10, "KXPR FM ", "Jazz all night long"},
    {91500, 18, false, 0, 0, NULL, NULL},
    {95500, 45, true, 0x5678, 5, "ROCK 955", "Now playing: Led Zeppelin - Kashmir"},
    {101100, 52, true, 0x9abc, 1, "NEWS 101", "Traffic and weather on the eights"},
    {103300, 9, false, 0, 0, NULL, NULL},   ///< too weak for a seek
    {107900, 28, true, 0x4321, 15, "CLASSIC ", "Mozart: Symphony No. 40 in G minor, K. 550 - Berlin Philharmonic"},
};

/// Band and spacing arithmetic.
static void test_channels(void) {
    si4703_config_t c;
    si4703_defaults(&c);
    static const uint8_t bands[3] = {SI4703_BAND_EU, SI4703_BAND_WIDE, SI4703_BAND_JP};
    static const uint8_t spaces[3] = {200, 100, 50};
    for (int b = 0; b < 3; ++b)
        for (int s = 0; s < 3; ++s) {
            c.band = bands[b];
            c.spacing = spaces[s];
            uint32_t lo = si4703_band_bottom(c.band), hi = si4703_band_top(c.band);
            for (uint32_t f = lo; f <= hi; f += c.spacing) {
                uint16_t ch = si4703_channel(&c, f);
                CHECK(si4703_channel_khz(&c, ch) == f, "band %d space %d: %u -> %u -> %u", b, s, f, ch,
                      si4703_channel_khz(&c, ch));
                CHECK(ch < 1024, "channel %u", ch);
            }
            CHECK(si4703_channel(&c, lo - 1000) == 0, "below the band");
            uint32_t top = si4703_channel_khz(&c, si4703_channel(&c, hi + 5000));
            CHECK(top <= hi && top > hi - c.spacing, "above the band: %u", top);
        }
    c.band = SI4703_BAND_EU;
    c.spacing = 200;
    CHECK(si4703_channel(&c, 101100) == 68, "101.1 at 200 kHz: %u", si4703_channel(&c, 101100));
    c.spacing = 100;
    CHECK(si4703_channel(&c, 101100) == 136, "101.1 at 100 kHz: %u", si4703_channel(&c, 101100));
    uint32_t k;
    CHECK(si4703_parse_freq("101.1", &k) && k == 101100, "101.1");
    CHECK(si4703_parse_freq("88.30 MHz", &k) && k == 88300, "88.30 MHz");
    CHECK(si4703_parse_freq("107900", &k) && k == 107900, "107900");
    CHECK(!si4703_parse_freq("12", &k), "12");
    char t[16];
    CHECK(!strcmp(si4703_freq_text(87500, t, sizeof t), "87.50"), "%s", t);
}

/// Power-up, tuning, seeking, settings.
static void test_chip(void) {
    si4703_bus_t bus = sim_bus();
    si4703_dev_t dev;
    sim_set_stations(air, sizeof air / sizeof *air);
    sim_present = false;
    CHECK(!si4703_init(&dev, &bus), "found without a chip");
    sim_present = true;
    sim_resets = 0;
    CHECK(si4703_init(&dev, &bus) && sim_resets == 1, "init");
    CHECK(!si4703_attach(&dev, &bus), "attached before power-up");
    si4703_config_t c;
    si4703_defaults(&c);
    CHECK(si4703_power_up(&dev, &c), "power up");
    const uint16_t *r = sim_regs();
    CHECK(r[7] & 0x8000, "crystal off");
    CHECK((r[2] & 0x4041) == 0x4001, "POWERCFG %04x", r[2]);
    CHECK(r[4] & 0x1000, "RDS off");
    CHECK(r[2] & 0x0800, "RDS not in verbose mode (no block errors)");
    CHECK(((r[5] >> 4) & 3) == 1, "spacing %d", (r[5] >> 4) & 3);
    CHECK(si4703_attach(&dev, &bus), "attach after power-up");
    // Volume: 0 mutes, 1-15 the quiet range (VOLEXT), 16-30 the loud one.
    c.volume = 0;
    si4703_configure(&dev, &c);
    CHECK((r[5] & 15) == 0 && !(r[2] & 0x4000), "volume 0: %04x %04x", r[5], r[2]);
    c.volume = 5;
    si4703_configure(&dev, &c);
    CHECK((r[5] & 15) == 5 && (r[6] & 0x100), "volume 5: %04x %04x", r[5], r[6]);
    c.volume = 30;
    si4703_configure(&dev, &c);
    CHECK((r[5] & 15) == 15 && !(r[6] & 0x100), "volume 30: %04x %04x", r[5], r[6]);
    c.mono = true;
    si4703_configure(&dev, &c);
    CHECK(r[2] & 0x2000, "mono");
    c.mono = false;
    si4703_configure(&dev, &c);
    // Tuning.
    si4703_status_t st;
    CHECK(si4703_tune(&dev, 101100), "tune");
    CHECK(si4703_status(&dev, &st) && st.khz == 101100 && st.rssi == 52 && st.stereo, "101.1: %u %u %d", st.khz,
          st.rssi, st.stereo);
    CHECK(!(r[3] & 0x8000), "TUNE left on");
    // Seeking up from 101.1 skips 103.3 (too weak) and finds 107.9; then the band's end.
    bool done = false, found = false;
    si4703_seek_start(&dev, true);
    for (int i = 0; i < 100 && !done; ++i)
        si4703_seek_poll(&dev, &done, &found);
    si4703_status(&dev, &st);
    CHECK(done && found && st.khz == 107900, "seek up: %d %d %u", done, found, st.khz);
    done = false;
    si4703_seek_start(&dev, true);
    for (int i = 0; i < 100 && !done; ++i)
        si4703_seek_poll(&dev, &done, &found);
    si4703_status(&dev, &st);
    CHECK(done && !found && st.khz == 108000, "seek past the top: %d %d %u", done, found, st.khz);
    done = false;
    si4703_seek_start(&dev, false);
    for (int i = 0; i < 100 && !done; ++i)
        si4703_seek_poll(&dev, &done, &found);
    si4703_status(&dev, &st);
    CHECK(done && found && st.khz == 107900, "seek down: %u", st.khz);
    CHECK(!(r[2] & 0x0100), "SEEK left on");
    // A scan: from the bottom, seek up until the end.
    si4703_tune(&dev, si4703_band_bottom(c.band));
    uint32_t got[16];
    int n = 0;
    for (;;) {
        done = false;
        si4703_seek_start(&dev, true);
        while (!done)
            si4703_seek_poll(&dev, &done, &found);
        if (!found)
            break;
        si4703_status(&dev, &st);
        got[n++] = st.khz;
    }
    CHECK(n == 4 && got[0] == 88100 && got[1] == 95500 && got[2] == 101100 && got[3] == 107900,
          "scan found %d: %u %u %u %u", n, got[0], got[1], got[2], got[3]);
    // Japan's band: 76-90 MHz.
    c.band = SI4703_BAND_JP;
    si4703_configure(&dev, &c);
    si4703_tune(&dev, 88100);
    si4703_status(&dev, &st);
    CHECK(st.khz == 88100 && st.rssi == 32, "Japan band 88.1: %u", st.khz);
    si4703_tune(&dev, 95500);  // outside: kept at the top
    si4703_status(&dev, &st);
    CHECK(st.khz == 90000, "Japan band top: %u", st.khz);
    c.band = SI4703_BAND_EU;
    si4703_configure(&dev, &c);
    CHECK(si4703_power_down(&dev) && (r[2] & 0x41) == 0x41, "power down %04x", r[2]);
}

/// RDS: names and text, with broken groups.
static void test_rds(void) {
    si4703_bus_t bus = sim_bus();
    si4703_dev_t dev;
    si4703_init(&dev, &bus);
    si4703_config_t c;
    si4703_defaults(&c);
    si4703_power_up(&dev, &c);
    for (int bad = 0; bad <= 3; bad += 3) {
        sim_bad_every = bad;
        for (unsigned k = 0; k < sizeof air / sizeof *air; ++k) {
            if (!air[k].ps)
                continue;
            si4703_tune(&dev, air[k].khz);
            si4703_rds_t rds;
            si4703_rds_clear(&rds);
            for (int i = 0; i < 600; ++i)
                si4703_rds_poll(&dev, &rds, NULL);
            CHECK(!strcmp(rds.ps, air[k].ps), "PS of %u (bad %d): '%s'", air[k].khz, bad, rds.ps);
            CHECK(!strcmp(rds.rt, air[k].rt), "RT of %u (bad %d): '%s'", air[k].khz, bad, rds.rt);
            CHECK(rds.pi == air[k].pi && rds.pty == air[k].pty, "PI %04x PTY %u", rds.pi, rds.pty);
        }
    }
    sim_bad_every = 0;
    // A group fed by hand: 2B, two characters at a time.
    si4703_rds_t rds;
    si4703_rds_clear(&rds);
    const char *text = "Hi there\r";
    for (int seg = 0; seg < 15; ++seg) {  // each segment twice, after the PI
        int k = seg % 5;
        uint16_t g[4] = {0x1111, (uint16_t)(0x2800 | k), 0x1111,
                         (uint16_t)((uint8_t)text[k * 2] << 8 | (uint8_t)(k * 2 + 1 < 9 ? text[k * 2 + 1] : ' '))};
        si4703_rds_group(&rds, g, NULL);
    }
    CHECK(!strcmp(rds.rt, "Hi there"), "2B text: '%s'", rds.rt);
    CHECK(!strcmp(si4703_pty_name(1, false), "News") && !strcmp(si4703_pty_name(9, true), "Top 40"), "PTY names");
}

/// Feeds a 0A group: two characters of the name.
/// @param rds the RDS state
/// @param pi block A
/// @param seg the segment (0-3)
/// @param ps the whole name
/// @param errors the blocks' errors, or NULL
static void feed_0a(si4703_rds_t *rds, uint16_t pi, int seg, const char *ps, const uint8_t errors[4]) {
    uint16_t g[4] = {pi, (uint16_t)(5 << 5 | seg), 0xe0cd,
                     (uint16_t)((uint8_t)ps[seg * 2] << 8 | (uint8_t)ps[seg * 2 + 1])};
    si4703_rds_group(rds, g, errors);
}

/// Feeds a 2A group: four characters of the text.
/// @param rds the RDS state
/// @param pi block A
/// @param seg the segment (0-15)
/// @param rt the text (64 characters, padded)
/// @param errors the blocks' errors, or NULL
static void feed_2a(si4703_rds_t *rds, uint16_t pi, int seg, const char *rt, const uint8_t errors[4]) {
    const uint8_t *t = (const uint8_t *)rt + seg * 4;
    uint16_t g[4] = {pi, (uint16_t)(0x2000 | 5 << 5 | seg), (uint16_t)(t[0] << 8 | t[1]),
                     (uint16_t)(t[2] << 8 | t[3])};
    si4703_rds_group(rds, g, errors);
}

/// RDS block errors, as at weak signal: 92.3 KOMP (PI 363F) once showed PI 5354.
static void test_rds_errors(void) {
    static const uint8_t ok[4] = {0, 0, 0, 0}, a_bad[4] = {3, 0, 0, 0}, a_fixed[4] = {2, 0, 0, 0},
                         b_bad[4] = {0, 3, 0, 0}, c_fixed[4] = {0, 0, 2, 0}, d_bad[4] = {0, 0, 0, 3},
                         d_fixed[4] = {0, 0, 0, 2};
    char rt[65];
    snprintf(rt, sizeof rt, "%-64s", "THE ROCK STATION\r");
    si4703_rds_t rds;
    si4703_rds_clear(&rds);
    // One block A is not enough for a PI, and nothing is taken before it.
    feed_0a(&rds, 0x363f, 0, "KOMP    ", ok);
    CHECK(rds.pi == 0 && rds.groups == 0, "PI after one group: %04x, %u groups", rds.pi, rds.groups);
    for (int i = 0; i < 2; ++i)
        for (int seg = 0; seg < 4; ++seg)
            feed_0a(&rds, 0x363f, seg, "KOMP    ", ok);
    CHECK(rds.pi == 0x363f && rds.pty == 5, "PI %04x PTY %u", rds.pi, rds.pty);
    CHECK(!strcmp(rds.ps, "KOMP    "), "PS '%s'", rds.ps);
    // A corrupt PI: uncorrectable, or miscorrected once, changes nothing,
    // and the group it came in is not taken.
    uint32_t groups = rds.groups;
    feed_0a(&rds, 0x5354, 1, "KOMP    ", a_bad);  // the rest of the group is good
    feed_0a(&rds, 0x5354, 1, "KOMP    ", a_bad);
    CHECK(rds.pi == 0x363f, "PI after uncorrectable A: %04x", rds.pi);
    feed_0a(&rds, 0x5354, 1, "XXXX    ", a_fixed);
    feed_0a(&rds, 0x363f, 2, "KOMP    ", ok);
    feed_0a(&rds, 0x5354, 1, "XXXX    ", a_fixed);
    CHECK(rds.pi == 0x363f && !strcmp(rds.ps, "KOMP    "), "PI after miscorrected A: %04x '%s'", rds.pi, rds.ps);
    CHECK(rds.groups == groups + 3, "groups with a bad PI taken: %u", rds.groups - groups);
    // Block B uncorrectable: the group is dropped.
    groups = rds.groups;
    feed_0a(&rds, 0x363f, 0, "XXXX    ", b_bad);
    CHECK(rds.groups == groups, "group with a bad B taken");
    // Characters: uncorrectable ones dropped; miscorrected ones (once) never shown.
    feed_0a(&rds, 0x363f, 1, "KOXX    ", d_bad);
    feed_0a(&rds, 0x363f, 1, "KOXX    ", d_fixed);
    for (int seg = 0; seg < 4; ++seg)
        feed_0a(&rds, 0x363f, seg, "KOMP    ", ok);
    CHECK(!strcmp(rds.ps, "KOMP    "), "PS after a miscorrected D: '%s'", rds.ps);
    // The text: each segment must come twice the same.
    for (int seg = 0; seg < 5; ++seg)
        feed_2a(&rds, 0x363f, seg, rt, ok);
    CHECK(rds.rt[0] == 0, "RT after one pass: '%s'", rds.rt);
    for (int seg = 0; seg < 5; ++seg)
        feed_2a(&rds, 0x363f, seg, rt, ok);
    CHECK(!strcmp(rds.rt, "THE ROCK STATION"), "RT '%s'", rds.rt);
    char wrong[65];
    memcpy(wrong, rt, sizeof wrong);
    wrong[1] = 'X';
    feed_2a(&rds, 0x363f, 0, wrong, c_fixed);
    for (int seg = 0; seg < 5; ++seg)
        feed_2a(&rds, 0x363f, seg, rt, ok);
    CHECK(!strcmp(rds.rt, "THE ROCK STATION"), "RT after a miscorrected C: '%s'", rds.rt);
    // A new PI, twice: another station, so the name and text start again.
    feed_0a(&rds, 0x1234, 0, "NEWS 101", ok);
    CHECK(rds.pi == 0x363f, "PI changed after one group: %04x", rds.pi);
    feed_0a(&rds, 0x1234, 0, "NEWS 101", ok);
    CHECK(rds.pi == 0x1234 && !rds.ps[0] && !rds.rt[0], "new PI %04x '%s' '%s'", rds.pi, rds.ps, rds.rt);
    for (int i = 0; i < 2; ++i)
        for (int seg = 0; seg < 4; ++seg)
            feed_0a(&rds, 0x1234, seg, "NEWS 101", ok);
    CHECK(!strcmp(rds.ps, "NEWS 101"), "new PS '%s'", rds.ps);
}

int main(void) {
    test_channels();
    test_chip();
    test_rds();
    test_rds_errors();
    printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures != 0;
}
