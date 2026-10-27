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
 *  - RDS: the station name and radio text, with broken groups mixed in.
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
            for (int i = 0; i < 400; ++i)
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
    for (int seg = 0; seg < 5; ++seg) {
        uint16_t g[4] = {0x1111, (uint16_t)(0x2800 | seg), 0x1111,
                         (uint16_t)((uint8_t)text[seg * 2] << 8 | (uint8_t)(seg * 2 + 1 < 9 ? text[seg * 2 + 1] : ' '))};
        si4703_rds_group(&rds, g);
    }
    CHECK(!strcmp(rds.rt, "Hi there"), "2B text: '%s'", rds.rt);
    CHECK(!strcmp(si4703_pty_name(1, false), "News") && !strcmp(si4703_pty_name(9, true), "Top 40"), "PTY names");
}

int main(void) {
    test_channels();
    test_chip();
    test_rds();
    printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures != 0;
}
