/**
 * @file si5351test.c
 * @brief PC tests of the Si5351 driver's frequency maths and registers.
 *
 *     cc -O2 -I../../src/si5351 si5351test.c ../../src/si5351/si5351.c -lm && ./a.out
 *
 *  - register values worked out by hand from AN619's formulas for 10 MHz,
 *    7.074 MHz, 144 MHz and 3 kHz;
 *  - a sweep from 2.5 kHz to 200 MHz (25 and 27 MHz crystals, with and
 *    without calibration, alone on a PLL and sharing one): every frequency
 *    within 1 Hz, VCOs 600-900 MHz, dividers in range;
 *  - a simulated chip: what si5351_apply writes is decoded back into
 *    frequencies independently of the planner;
 *  - quadrature: CLK1 90 degrees behind CLK0 (PHOFF equal to the divider).
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "si5351.h"

static int failures;  ///< checks that failed

/// Reports a failed check.
#define CHECK(cond, ...)                                              \
    do {                                                              \
        if (!(cond)) {                                                \
            ++failures;                                               \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);               \
            printf(__VA_ARGS__);                                      \
            printf("\n");                                             \
        }                                                             \
    } while (0)

// ---- A simulated chip --------------------------------------------------------------------------

static uint8_t chip[256];  ///< its registers
static int writes;         ///< write transactions seen
static int resets;         ///< PLL resets seen (register 177 written)

/// The simulated chip's write.
/// @param ctx unused
/// @param reg the first register
/// @param d the bytes
/// @param n their count
/// @return true
static bool sim_write(void *ctx, uint8_t reg, const uint8_t *d, int n) {
    (void)ctx;
    ++writes;
    if (reg == 177)
        ++resets;
    memcpy(chip + reg, d, n);
    return true;
}

/// The simulated chip's read.
/// @param ctx unused
/// @param reg the first register
/// @param d where
/// @param n how many
/// @return true
static bool sim_read(void *ctx, uint8_t reg, uint8_t *d, int n) {
    (void)ctx;
    memcpy(d, chip + reg, n);
    return true;
}

/// a + b/c from 8 parameter registers (AN619's P1, P2, P3 undone).
/// @param r the registers
/// @return the ratio's value
static double decode(const uint8_t *r) {
    uint32_t p3 = (uint32_t)(r[5] >> 4) << 16 | r[0] << 8 | r[1];
    uint32_t p1 = (uint32_t)(r[2] & 3) << 16 | r[3] << 8 | r[4];
    uint32_t p2 = (uint32_t)(r[5] & 15) << 16 | r[6] << 8 | r[7];
    return (p1 + 512) / 128.0 + (double)p2 / (128.0 * p3);
}

/// The frequency an output makes, from the chip's registers alone.
/// @param xtal the crystal's real frequency
/// @param i the output
/// @return Hz, or 0 if it is off
static double chip_freq(double xtal, int i) {
    uint8_t ctrl = chip[16 + i];
    if ((ctrl & 0x80) || (chip[3] & (1 << i)))
        return 0;
    double vco = xtal * decode(chip + 26 + ((ctrl & 0x20) ? 8 : 0));
    const uint8_t *ms = chip + 42 + 8 * i;
    double d = (ms[2] & 0x0c) == 0x0c ? 4 : decode(ms);
    return vco / d / (1 << ((ms[2] >> 4) & 7));
}

// ---- Tests -------------------------------------------------------------------------------------

/// Compares 8 registers with the values expected.
/// @param what the test's name
/// @param got the registers
/// @param want the values
static void expect8(const char *what, const uint8_t *got, const uint8_t *want) {
    CHECK(!memcmp(got, want, 8), "%s: %02x %02x %02x %02x %02x %02x %02x %02x, want %02x %02x %02x %02x %02x %02x %02x %02x",
          what, got[0], got[1], got[2], got[3], got[4], got[5], got[6], got[7],
          want[0], want[1], want[2], want[3], want[4], want[5], want[6], want[7]);
}

/// Register values for single frequencies on CLK0 / PLL A, 25 MHz crystal.
static void test_reference(void) {
    static const struct {
        uint32_t hz;
        uint8_t pll[8], ms[8], ctrl;
    } t[] = {
        // 10 MHz: VCO 600 MHz = 25 x 24, MS 60.
        {10000000, {0x00, 0x01, 0x00, 0x0a, 0x00, 0x00, 0x00, 0x00},
         {0x00, 0x01, 0x00, 0x1c, 0x00, 0x00, 0x00, 0x00}, 0x4f},
        // 7.074 MHz: MS 86, VCO 608.364 MHz = 25 x (24 + 2091/6250).
        {7074000, {0x18, 0x6a, 0x00, 0x0a, 0x2a, 0x00, 0x14, 0x1c},
         {0x00, 0x01, 0x00, 0x29, 0x00, 0x00, 0x00, 0x00}, 0x4f},
        // 144 MHz: MS 6, VCO 864 MHz = 25 x (34 + 14/25).
        {144000000, {0x00, 0x19, 0x00, 0x0f, 0x47, 0x00, 0x00, 0x11},
         {0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00}, 0x4f},
        // 3 kHz: R 128, MS 1564, VCO 600.576 MHz = 25 x (24 + 72/3125).
        {3000, {0x0c, 0x35, 0x00, 0x0a, 0x02, 0x00, 0x0b, 0x96},
         {0x00, 0x01, 0x73, 0x0c, 0x00, 0x00, 0x00, 0x00}, 0x4f},
    };
    for (unsigned k = 0; k < sizeof t / sizeof *t; ++k) {
        si5351_config_t cfg;
        si5351_defaults(&cfg);
        cfg.out[0].freq_hz = t[k].hz;
        si5351_plan_t plan;
        uint8_t regs[SI5351_REGS];
        CHECK(si5351_plan(&cfg, &plan), "%u Hz: no plan", t[k].hz);
        si5351_registers(&cfg, &plan, regs);
        char name[40];
        snprintf(name, sizeof name, "%u Hz PLL A", t[k].hz);
        expect8(name, regs + 26, t[k].pll);
        snprintf(name, sizeof name, "%u Hz MS0", t[k].hz);
        expect8(name, regs + 42, t[k].ms);
        CHECK(regs[16] == t[k].ctrl, "%u Hz: CLK0 control %02x, want %02x", t[k].hz, regs[16], t[k].ctrl);
        CHECK(regs[3] == 0xfe, "%u Hz: output enable %02x", t[k].hz, regs[3]);
        CHECK(fabs(plan.out[0].actual_hz - t[k].hz) < 1e-6, "%u Hz: makes %.6f", t[k].hz, plan.out[0].actual_hz);
    }
    // 200 MHz: DIVBY4 (P1 = P2 = 0, P3 = 1), VCO 800 MHz = 25 x 32.
    si5351_config_t cfg;
    si5351_defaults(&cfg);
    cfg.out[0].freq_hz = 200000000;
    si5351_plan_t plan;
    uint8_t regs[SI5351_REGS];
    si5351_plan(&cfg, &plan);
    si5351_registers(&cfg, &plan, regs);
    static const uint8_t ms4[8] = {0x00, 0x01, 0x0c, 0x00, 0x00, 0x00, 0x00, 0x00};
    static const uint8_t pll32[8] = {0x00, 0x01, 0x00, 0x0e, 0x00, 0x00, 0x00, 0x00};
    expect8("200 MHz MS0", regs + 42, ms4);
    expect8("200 MHz PLL A", regs + 26, pll32);
    CHECK(regs[22] & 0x40, "200 MHz: FBA_INT not set");
    CHECK(regs[183] == 0xd2, "10 pF load: %02x", regs[183]);
}

/// The sweep: every frequency within 1 Hz, everything in range.
static void test_sweep(void) {
    static const uint32_t xtals[] = {25000000, 27000000};
    static const int32_t cals[] = {0, 12345, -87654};
    int n = 0;
    double worst = 0;
    for (unsigned x = 0; x < 2; ++x)
        for (unsigned c = 0; c < 3; ++c)
            for (double f = SI5351_MIN_HZ; f <= SI5351_MAX_HZ; f *= 1.0137) {
                uint32_t hz = (uint32_t)f + (n % 977);  // odd values too
                if (hz > SI5351_MAX_HZ)
                    hz = SI5351_MAX_HZ;
                si5351_config_t cfg;
                si5351_defaults(&cfg);
                cfg.xtal_hz = xtals[x];
                cfg.cal_ppb = cals[c];
                // CLK0 alone on A; CLK1 sets B; CLK2 shares B at another frequency.
                uint32_t shared = (uint32_t)(hz * 0.7317) < SI5351_MIN_HZ ? SI5351_MIN_HZ : (uint32_t)(hz * 0.7317);
                cfg.out[0] = (si5351_out_t){hz, true, 3, false, 0, 0};
                cfg.out[1] = (si5351_out_t){shared, true, 3, false, 1, 0};
                cfg.out[2] = (si5351_out_t){hz > 100000000 ? hz / 5 : hz, true, 3, false, 1, 0};
                si5351_plan_t plan;
                si5351_plan(&cfg, &plan);
                for (int i = 0; i < 3; ++i) {
                    const si5351_out_plan_t *o = &plan.out[i];
                    if (o->error) {
                        // Sharing may legitimately fail (too high for a shared VCO).
                        CHECK(i == 2, "%u Hz CLK%d: %s", cfg.out[i].freq_hz, i, o->error);
                        continue;
                    }
                    double err = fabs(o->actual_hz - cfg.out[i].freq_hz);
                    if (err > worst)
                        worst = err;
                    CHECK(err < 1.0, "xtal %u cal %d: CLK%d %u Hz makes %.4f", xtals[x], cals[c], i,
                          cfg.out[i].freq_hz, o->actual_hz);
                    double ms = o->ms.a + (double)o->ms.b / o->ms.c;
                    CHECK(o->divby4 || ms == 6 || (ms >= 8 && ms <= 2048), "CLK%d %u Hz: MS %f", i,
                          cfg.out[i].freq_hz, ms);
                    CHECK(o->ms.c <= SI5351_FRAC_MAX && o->ms.b < o->ms.c, "MS fraction %u/%u", o->ms.b, o->ms.c);
                }
                for (int p = 0; p < 2; ++p) {
                    CHECK(plan.vco_hz[p] >= SI5351_VCO_MIN - 1 && plan.vco_hz[p] <= SI5351_VCO_MAX + 1,
                          "%u Hz: VCO %c %.0f", hz, 'A' + p, plan.vco_hz[p]);
                    CHECK(plan.pll[p].a >= 15 && plan.pll[p].a <= 90 && plan.pll[p].c <= SI5351_FRAC_MAX,
                          "PLL %c %u + %u/%u", 'A' + p, plan.pll[p].a, plan.pll[p].b, plan.pll[p].c);
                }
                ++n;
            }
    printf("sweep: %d setups, worst error %.6f Hz\n", n, worst);
}

/// The simulated chip: decoding what was written gives the frequencies.
static void test_chip(void) {
    si5351_bus_t bus = {sim_write, sim_read, NULL};
    si5351_dev_t dev;
    si5351_init(&dev, &bus);
    memset(chip, 0, sizeof chip);
    si5351_config_t cfg;
    si5351_defaults(&cfg);
    cfg.cal_ppb = 3500;
    cfg.out[1].enabled = cfg.out[2].enabled = true;
    si5351_plan_t plan;
    writes = resets = 0;
    CHECK(si5351_apply(&dev, &cfg, &plan), "apply failed");
    CHECK(resets == 1, "first apply: %d resets", resets);
    double xtal = cfg.xtal_hz * (1 + cfg.cal_ppb / 1e9);
    for (int i = 0; i < 3; ++i)
        CHECK(fabs(chip_freq(xtal, i) - cfg.out[i].freq_hz) < 1, "chip CLK%d: %.4f", i, chip_freq(xtal, i));
    // Fine tuning CLK0 (alone on A): just PLL A's registers, no reset.
    writes = resets = 0;
    cfg.out[0].freq_hz += 1;
    si5351_apply(&dev, &cfg, &plan);
    CHECK(resets == 0 && writes == 1, "tune by 1 Hz: %d writes, %d resets", writes, resets);
    CHECK(fabs(chip_freq(xtal, 0) - cfg.out[0].freq_hz) < 1, "tuned: %.4f", chip_freq(xtal, 0));
    // Off: the enable register and CLK0's control.
    cfg.out[0].enabled = false;
    si5351_apply(&dev, &cfg, &plan);
    CHECK(chip_freq(xtal, 0) == 0 && (chip[3] & 1), "CLK0 still on");
    // A new divider resets its PLL.
    cfg.out[0].enabled = true;
    cfg.out[0].freq_hz = 50000000;
    resets = 0;
    si5351_apply(&dev, &cfg, &plan);
    CHECK(resets == 1 && chip[177] == 0x20, "new divider: %d resets (%02x)", resets, chip[177]);
    CHECK(fabs(chip_freq(xtal, 0) - 50e6) < 1, "50 MHz: %.4f", chip_freq(xtal, 0));
    // Drive and invert.
    cfg.out[2].drive = 1;
    cfg.out[2].invert = true;
    si5351_apply(&dev, &cfg, &plan);
    CHECK((chip[18] & 0x13) == 0x11 && (chip[18] & 0x20), "CLK2 control %02x", chip[18]);
}

/// Quadrature: CLK1 90 degrees behind CLK0.
static void test_iq(void) {
    static const uint32_t f[] = {4800000, 7074000, 14074000, 28000000, 50000000, 100000000};
    for (unsigned k = 0; k < sizeof f / sizeof *f; ++k) {
        si5351_config_t cfg;
        si5351_defaults(&cfg);
        cfg.iq = true;
        cfg.out[0].freq_hz = f[k];
        cfg.out[1].enabled = true;
        si5351_plan_t plan;
        CHECK(si5351_plan(&cfg, &plan), "I/Q %u: %s", f[k], plan.out[1].error ? plan.out[1].error : "?");
        CHECK(plan.out[1].pll == plan.out[0].pll, "I/Q %u: different PLLs", f[k]);
        CHECK(plan.out[0].integer && plan.out[1].integer && plan.out[0].ms.a == plan.out[1].ms.a,
              "I/Q %u: dividers %u %u", f[k], plan.out[0].ms.a, plan.out[1].ms.a);
        CHECK(plan.out[1].phoff == plan.out[0].ms.a && plan.out[0].phoff == 0, "I/Q %u: PHOFF %u, MS %u", f[k],
              plan.out[1].phoff, plan.out[0].ms.a);
        CHECK(fabs(plan.out[1].phase_deg - 90) < 1e-9, "I/Q %u: phase %f", f[k], plan.out[1].phase_deg);
        CHECK(fabs(plan.out[1].actual_hz - f[k]) < 1, "I/Q %u: CLK1 %.3f", f[k], plan.out[1].actual_hz);
    }
    si5351_config_t cfg;
    si5351_defaults(&cfg);
    cfg.iq = true;
    cfg.out[0].freq_hz = 1000000;
    cfg.out[1].enabled = true;
    si5351_plan_t plan;
    CHECK(!si5351_plan(&cfg, &plan), "I/Q at 1 MHz should fail");
}

/// Best ratios and the text conversions.
static void test_misc(void) {
    uint32_t n, d;
    si5351_best_ratio(2091.0 / 6250, SI5351_FRAC_MAX, &n, &d);
    CHECK(n == 2091 && d == 6250, "2091/6250: %u/%u", n, d);
    si5351_best_ratio(M_PI - 3, 1000, &n, &d);
    CHECK(n == 16 && d == 113, "pi - 3 within 1000: %u/%u", n, d);  // 355/113
    si5351_best_ratio(0, 100, &n, &d);
    CHECK(n == 0 && d == 1, "0: %u/%u", n, d);
    uint32_t hz;
    CHECK(si5351_parse_freq("10M", &hz) && hz == 10000000, "10M");
    CHECK(si5351_parse_freq("7.074 MHz", &hz) && hz == 7074000, "7.074 MHz");
    CHECK(si5351_parse_freq("14074k", &hz) && hz == 14074000, "14074k");
    CHECK(si5351_parse_freq("2500", &hz) && hz == 2500, "2500");
    CHECK(!si5351_parse_freq("10X", &hz), "10X");
    char t[32];
    CHECK(!strcmp(si5351_freq_text(7074000, t, sizeof t), "7.074 MHz"), "%s", t);
    CHECK(!strcmp(si5351_freq_text(2500, t, sizeof t), "2.5 kHz"), "%s", t);
    CHECK(!strcmp(si5351_freq_text(10000000, t, sizeof t), "10 MHz"), "%s", t);
}

int main(void) {
    test_reference();
    test_sweep();
    test_chip();
    test_iq();
    test_misc();
    printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures != 0;
}
