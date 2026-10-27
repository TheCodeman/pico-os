/**
 * @file si5351.c
 * @brief Implementation of si5351.h: frequency planning, the register image and writing it.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */
#include "si5351.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/// Registers (AN619).
enum {
    REG_STATUS = 0,       ///< SYS_INIT, LOL_B, LOL_A, LOS
    REG_OUTPUT_EN = 3,    ///< a bit per output: 1 disables it
    REG_PLL_SRC = 15,     ///< the PLLs' input: 0 the crystal for both
    REG_CLK0_CTRL = 16,   ///< CLK0-CLK7 control, one each
    REG_CLK6_CTRL = 22,   ///< (bit 6: PLL A in integer mode)
    REG_CLK7_CTRL = 23,   ///< (bit 6: PLL B in integer mode)
    REG_PLLA = 26,        ///< PLL A's 8 parameter bytes (PLL B's follow)
    REG_MS0 = 42,         ///< multisynth 0's 8 bytes (MS1 and MS2 follow)
    REG_CLK0_PHOFF = 165, ///< CLK0-CLK2 phase offsets
    REG_PLL_RESET = 177,  ///< bit 7 resets PLL B, bit 5 PLL A
    REG_XTAL_LOAD = 183,  ///< crystal load capacitance
};

/// CLKx control bits.
enum {
    CLK_PDN = 0x80,     ///< powered down
    CLK_INT = 0x40,     ///< multisynth in integer mode
    CLK_SRC_B = 0x20,   ///< fed from PLL B (else A)
    CLK_INV = 0x10,     ///< inverted
    CLK_SRC_MS = 0x0c,  ///< driven by its own multisynth
};

/// The register blocks the driver owns, as first register and count.
static const uint8_t blocks[][2] = {
    {REG_OUTPUT_EN, 1}, {REG_PLL_SRC, 1}, {REG_CLK0_CTRL, 8}, {REG_PLLA, 8}, {REG_PLLA + 8, 8},
    {REG_MS0, 8}, {REG_MS0 + 8, 8}, {REG_MS0 + 16, 8}, {REG_CLK0_PHOFF, 3}, {REG_XTAL_LOAD, 1},
};
#define NBLOCKS (int)(sizeof blocks / sizeof *blocks)  ///< their count

void si5351_defaults(si5351_config_t *cfg) {
    memset(cfg, 0, sizeof *cfg);
    cfg->xtal_hz = 25000000;
    cfg->load_pf = 10;
    static const uint32_t f[SI5351_OUTPUTS] = {10000000, 7074000, 14074000};
    for (int i = 0; i < SI5351_OUTPUTS; ++i) {
        cfg->out[i].freq_hz = f[i];
        cfg->out[i].enabled = i == 0;
        cfg->out[i].drive = 3;
        cfg->out[i].pll = i ? 1 : 0;
    }
}

void si5351_best_ratio(double x, uint32_t max_den, uint32_t *num, uint32_t *den) {
    // Convergents h/k of x's continued fraction, until the denominator would
    // pass max_den; then the best semiconvergent below that.
    uint64_t h0 = 0, h1 = 1, k0 = 1, k1 = 0;
    double v = x;
    for (int i = 0; i < 64; ++i) {
        double a = floor(v);
        uint64_t ai = (uint64_t)a;
        uint64_t h2 = ai * h1 + h0, k2 = ai * k1 + k0;
        if (k2 > max_den) {
            // Semiconvergents (h0 + t h1)/(k0 + t k1), t <= (max_den - k0)/k1:
            // the largest one is better than h1/k1 if it is nearer x.
            uint64_t t = (max_den - k0) / k1;
            if (t * 2 >= ai) {
                uint64_t hs = h0 + t * h1, ks = k0 + t * k1;
                if (fabs((double)hs / ks - x) < fabs((double)h1 / k1 - x)) {
                    h1 = hs;
                    k1 = ks;
                }
            }
            break;
        }
        h0 = h1;
        h1 = h2;
        k0 = k1;
        k1 = k2;
        double f = v - a;
        if (f < 1e-12 || fabs((double)h1 / k1 - x) < 1e-15)
            break;
        v = 1 / f;
    }
    if (k1 == 0) {  // (can't happen for x in [0, 1); be safe)
        h1 = 0;
        k1 = 1;
    }
    if (h1 >= k1) {  // rounded up to 1: the nearest below it
        h1 = max_den - 1;
        k1 = max_den;
    }
    *num = (uint32_t)h1;
    *den = (uint32_t)k1;
}

/// A ratio for a value: its whole part and the best fraction.
/// @param x the value (positive)
/// @return the ratio
static si5351_ratio_t ratio_of(double x) {
    si5351_ratio_t r;
    r.a = (uint32_t)floor(x);
    si5351_best_ratio(x - r.a, SI5351_FRAC_MAX, &r.b, &r.c);
    if (r.b == 0)
        r.c = 1;
    return r;
}

/// A ratio's value.
/// @param r the ratio
/// @return a + b/c
static double ratio_value(si5351_ratio_t r) {
    return r.a + (double)r.b / r.c;
}

/// The R divider for a frequency: the smallest that brings it to 500 kHz or
/// more (and the multisynth to 2048 or less on a VCO of vco).
/// @param f the frequency
/// @param vco the VCO frequency, or 0 for the lowest (600 MHz)
/// @return log2 of the divider (0-7)
static uint8_t r_for(double f, double vco) {
    if (vco <= 0)
        vco = SI5351_VCO_MIN;
    uint8_t r = 0;
    while (r < 7 && (f * (1 << r) < 500000 || vco / (f * (1 << r)) > 2048))
        ++r;
    return r;
}

/// Plans an output that sets its PLL's VCO: an integer divider, the PLL
/// fractional.
/// @param plan the plan (its PLL and the output)
/// @param i the output
/// @param f its frequency
/// @param max_d the largest divider allowed (for phase offsets), or 0
static void plan_reference(si5351_plan_t *plan, int i, double f, uint32_t max_d) {
    si5351_out_plan_t *o = &plan->out[i];
    o->r_log2 = r_for(f, 0);
    double fms = f * (1 << o->r_log2);
    uint32_t d;
    if (fms > 150000000) {
        d = 4;
        o->divby4 = true;
    } else if (fms > 112500000) {
        d = 6;
    } else {
        d = (uint32_t)ceil(SI5351_VCO_MIN / fms - 1e-9);
        d += d & 1;
        if (d < 8)
            d = 8;
    }
    if (max_d && d > max_d) {
        o->error = "below 4.8 MHz: too low for I/Q";
        return;
    }
    o->ms = (si5351_ratio_t){d, 0, 1};
    o->integer = true;
    int p = o->pll;
    plan->pll[p] = ratio_of(d * fms / plan->xtal_hz);
    plan->vco_hz[p] = plan->xtal_hz * ratio_value(plan->pll[p]);
    plan->pll_used[p] = true;
}

/// Plans an output on a VCO another output set: a fractional divider.
/// @param plan the plan
/// @param i the output
/// @param f its frequency
static void plan_follower(si5351_plan_t *plan, int i, double f) {
    si5351_out_plan_t *o = &plan->out[i];
    double vco = plan->vco_hz[o->pll];
    o->r_log2 = r_for(f, vco);
    double ms = vco / (f * (1 << o->r_log2));
    if (ms < 8) {
        if (fabs(ms - 4) < 1e-9 || fabs(ms - 6) < 1e-9) {  // exactly 4 or 6
            uint32_t d = (uint32_t)lround(ms);
            o->ms = (si5351_ratio_t){d, 0, 1};
            o->integer = true;
            o->divby4 = d == 4;
            return;
        }
        o->error = "too high to share a PLL: use the other";
        return;
    }
    if (ms > 2048) {
        o->error = "too low for this PLL";
        return;
    }
    o->ms = ratio_of(ms);
    if (o->ms.a >= 2048 && o->ms.b) {
        o->ms = (si5351_ratio_t){2048, 0, 1};
    }
    o->integer = o->ms.b == 0;
}

bool si5351_plan(const si5351_config_t *cfg, si5351_plan_t *plan) {
    memset(plan, 0, sizeof *plan);
    plan->xtal_hz = cfg->xtal_hz * (1.0 + cfg->cal_ppb / 1e9);
    // The outputs as they will be: quadrature ties CLK1 to CLK0.
    si5351_out_t out[SI5351_OUTPUTS];
    memcpy(out, cfg->out, sizeof out);
    if (cfg->iq) {
        out[1].freq_hz = out[0].freq_hz;
        out[1].pll = out[0].pll;
        out[0].phase = 0;
        out[1].phase = 90;
    }
    for (int i = 0; i < SI5351_OUTPUTS; ++i)
        plan->out[i].pll = out[i].pll & 1;
    // Each PLL: the first output on it sets its VCO, the rest follow.
    for (int p = 0; p < 2; ++p) {
        bool have_ref = false;
        for (int i = 0; i < SI5351_OUTPUTS; ++i) {
            if (!out[i].enabled || plan->out[i].pll != p)
                continue;
            double f = out[i].freq_hz;
            if (out[i].freq_hz < SI5351_MIN_HZ || out[i].freq_hz > SI5351_MAX_HZ) {
                plan->out[i].error = "out of range (2.5 kHz-200 MHz)";
                continue;
            }
            if (!have_ref) {
                plan_reference(plan, i, f, cfg->iq && i == 0 ? 126 : 0);
                have_ref = plan->out[i].error == NULL;
            } else {
                plan_follower(plan, i, f);
            }
        }
    }
    for (int p = 0; p < 2; ++p)
        if (!plan->pll_used[p]) {  // a valid setting all the same: 600 MHz
            plan->pll[p] = ratio_of(SI5351_VCO_MIN / plan->xtal_hz);
            plan->vco_hz[p] = plan->xtal_hz * ratio_value(plan->pll[p]);
        }
    bool ok = true;
    for (int i = 0; i < SI5351_OUTPUTS; ++i) {
        si5351_out_plan_t *o = &plan->out[i];
        if (!out[i].enabled) {
            o->error = NULL;
            continue;
        }
        if (o->error) {
            ok = false;
            continue;
        }
        double ms = ratio_value(o->ms);
        o->actual_hz = plan->vco_hz[o->pll] / ms / (1 << o->r_log2);
        // Phase: quarter VCO periods, which need an integer divider (and are
        // in multisynth periods: with an R divider they are divided again).
        if (out[i].phase % 360) {
            if (!o->integer || o->divby4) {
                o->phase_deg = 0;
            } else {
                double q = (out[i].phase % 360) / 360.0 * 4 * o->ms.a;
                long v = lround(q);
                o->phoff = (uint8_t)(v > 127 ? 127 : v);
                o->phase_deg = o->phoff * 90.0 / o->ms.a / (1 << o->r_log2);
            }
        }
    }
    if (cfg->iq && !plan->out[0].error && plan->out[1].phoff != plan->out[0].ms.a && out[1].enabled) {
        plan->out[1].error = "I/Q needs CLK0 at 4.8-112.5 MHz";
        ok = false;
    }
    return ok;
}

void si5351_params(si5351_ratio_t r, uint32_t *p1, uint32_t *p2, uint32_t *p3) {
    uint32_t f = (uint32_t)((128ull * r.b) / r.c);
    *p1 = 128 * r.a + f - 512;
    *p2 = 128 * r.b - r.c * f;
    *p3 = r.c;
}

/// Puts a ratio's parameters into 8 registers, in the PLL and multisynth
/// layout (byte 2 also takes extra: the R divider and DIVBY4).
/// @param r8 the 8 registers
/// @param r the ratio
/// @param extra bits for byte 2
static void put_params(uint8_t *r8, si5351_ratio_t r, uint8_t extra) {
    uint32_t p1, p2, p3;
    si5351_params(r, &p1, &p2, &p3);
    r8[0] = (uint8_t)(p3 >> 8);
    r8[1] = (uint8_t)p3;
    r8[2] = (uint8_t)(extra | ((p1 >> 16) & 3));
    r8[3] = (uint8_t)(p1 >> 8);
    r8[4] = (uint8_t)p1;
    r8[5] = (uint8_t)(((p3 >> 12) & 0xf0) | ((p2 >> 16) & 0x0f));
    r8[6] = (uint8_t)(p2 >> 8);
    r8[7] = (uint8_t)p2;
}

void si5351_registers(const si5351_config_t *cfg, const si5351_plan_t *plan, uint8_t regs[SI5351_REGS]) {
    memset(regs, 0, SI5351_REGS);
    uint8_t oe = 0xff;
    for (int i = 0; i < 8; ++i)
        regs[REG_CLK0_CTRL + i] = CLK_PDN;
    for (int i = 0; i < SI5351_OUTPUTS; ++i) {
        const si5351_out_plan_t *o = &plan->out[i];
        const si5351_out_t *c = &cfg->out[i];
        bool on = c->enabled && !o->error;
        uint8_t ctrl = (uint8_t)(CLK_SRC_MS | (c->drive & 3));
        if (!on)
            ctrl |= CLK_PDN;
        if (o->integer)
            ctrl |= CLK_INT;
        if (o->pll)
            ctrl |= CLK_SRC_B;
        if (c->invert)
            ctrl |= CLK_INV;
        regs[REG_CLK0_CTRL + i] = ctrl;
        if (on)
            oe &= (uint8_t)~(1 << i);
        si5351_ratio_t ms = o->ms;
        uint8_t extra = (uint8_t)(o->r_log2 << 4);
        if (o->divby4) {
            ms = (si5351_ratio_t){4, 0, 1};
            extra |= 0x0c;
        }
        if (!on || ms.a < 4)  // off: a valid divider all the same
            ms = (si5351_ratio_t){8, 0, 1};
        put_params(regs + REG_MS0 + 8 * i, ms, extra);
        if (o->divby4) {  // DIVBY4: P1 = P2 = 0, P3 = 1
            uint8_t *m = regs + REG_MS0 + 8 * i;
            m[2] &= 0xfc;
            m[3] = m[4] = m[5] = m[6] = m[7] = 0;
            m[0] = 0;
            m[1] = 1;
        }
        regs[REG_CLK0_PHOFF + i] = o->phoff & 0x7f;
    }
    regs[REG_OUTPUT_EN] = oe;
    regs[REG_PLL_SRC] = 0;
    for (int p = 0; p < 2; ++p) {
        put_params(regs + REG_PLLA + 8 * p, plan->pll[p], 0);
        if (plan->pll[p].b == 0)
            regs[REG_CLK6_CTRL + p] |= CLK_INT;  // FBA_INT / FBB_INT
    }
    uint8_t load = cfg->load_pf <= 6 ? 1 : cfg->load_pf <= 8 ? 2 : 3;
    regs[REG_XTAL_LOAD] = (uint8_t)(load << 6 | 0x12);
}

void si5351_init(si5351_dev_t *dev, const si5351_bus_t *bus) {
    memset(dev, 0, sizeof *dev);
    dev->bus = *bus;
}

void si5351_invalidate(si5351_dev_t *dev) {
    dev->written = false;
}

bool si5351_probe(si5351_dev_t *dev, uint8_t *status) {
    uint8_t s;
    if (!dev->bus.read(dev->bus.ctx, REG_STATUS, &s, 1))
        return false;
    if (status)
        *status = s;
    return true;
}

/// Writes one register.
/// @param dev the chip
/// @param reg the register
/// @param v its value
/// @return the chip answered
static bool write1(si5351_dev_t *dev, uint8_t reg, uint8_t v) {
    return dev->bus.write(dev->bus.ctx, reg, &v, 1);
}

/// Writes a block of the image.
/// @param dev the chip
/// @param regs the image
/// @param b the block
/// @return the chip answered
static bool write_block(si5351_dev_t *dev, const uint8_t *regs, int b) {
    return dev->bus.write(dev->bus.ctx, blocks[b][0], regs + blocks[b][0], blocks[b][1]);
}

/// Whether a block differs from what the chip has.
/// @param dev the chip
/// @param regs the image
/// @param b the block
/// @return it differs
static bool block_changed(const si5351_dev_t *dev, const uint8_t *regs, int b) {
    return memcmp(dev->shadow + blocks[b][0], regs + blocks[b][0], blocks[b][1]) != 0;
}

bool si5351_read_back(si5351_dev_t *dev) {
    for (int b = 0; b < NBLOCKS; ++b)
        if (!dev->bus.read(dev->bus.ctx, blocks[b][0], dev->shadow + blocks[b][0], blocks[b][1])) {
            dev->written = false;
            return false;
        }
    dev->written = true;
    return true;
}

bool si5351_all_off(si5351_dev_t *dev) {
    bool ok = write1(dev, REG_OUTPUT_EN, 0xff);
    for (int i = 0; ok && i < SI5351_OUTPUTS; ++i) {
        uint8_t v = dev->shadow[REG_CLK0_CTRL + i] | CLK_PDN;
        ok = write1(dev, REG_CLK0_CTRL + i, v);
        dev->shadow[REG_CLK0_CTRL + i] = v;
    }
    dev->shadow[REG_OUTPUT_EN] = 0xff;
    return ok;
}

bool si5351_apply(si5351_dev_t *dev, const si5351_config_t *cfg, si5351_plan_t *plan_out) {
    si5351_plan_t plan_local, *plan = plan_out ? plan_out : &plan_local;
    si5351_plan(cfg, plan);
    uint8_t regs[SI5351_REGS];
    si5351_registers(cfg, plan, regs);
    bool ok = true;
    if (!dev->written) {
        // From scratch (AN619): outputs off, drivers down, everything in,
        // PLLs reset, outputs on.
        ok = write1(dev, REG_OUTPUT_EN, 0xff);
        static const uint8_t down[8] = {CLK_PDN, CLK_PDN, CLK_PDN, CLK_PDN,
                                        CLK_PDN, CLK_PDN, CLK_PDN, CLK_PDN};
        ok = ok && dev->bus.write(dev->bus.ctx, REG_CLK0_CTRL, down, 8);
        for (int b = 1; ok && b < NBLOCKS; ++b)
            ok = write_block(dev, regs, b);
        ok = ok && write1(dev, REG_PLL_RESET, 0xa0);
        ok = ok && write_block(dev, regs, 0);
        if (ok) {
            memcpy(dev->shadow, regs, SI5351_REGS);
            dev->written = true;
        }
        return ok;
    }
    // Only what changed. A changed divider, phase or output setting resets
    // the PLL feeding that output (to line the phases up again); a PLL
    // retuned on its own just moves (no reset: no glitch).
    uint8_t reset = 0;
    for (int i = 0; i < SI5351_OUTPUTS; ++i) {
        bool changed = block_changed(dev, regs, 5 + i) ||
                       dev->shadow[REG_CLK0_CTRL + i] != regs[REG_CLK0_CTRL + i] ||
                       dev->shadow[REG_CLK0_PHOFF + i] != regs[REG_CLK0_PHOFF + i];
        if (changed && cfg->out[i].enabled && !plan->out[i].error)
            reset |= plan->out[i].pll ? 0x80 : 0x20;
    }
    static const int order[] = {9, 1, 3, 4, 5, 6, 7, 2, 8, 0};  // load, src, PLLs, MSs, ctrl, phase, OE
    for (int k = 0; ok && k < NBLOCKS; ++k) {
        int b = order[k];
        if (b == 0 && reset)
            ok = write1(dev, REG_PLL_RESET, reset);
        if (ok && block_changed(dev, regs, b)) {
            ok = write_block(dev, regs, b);
            if (ok)
                memcpy(dev->shadow + blocks[b][0], regs + blocks[b][0], blocks[b][1]);
        }
    }
    if (!ok)
        dev->written = false;  // who knows what it has now
    return ok;
}

char *si5351_freq_text(double hz, char *out, int size) {
    const char *unit = "Hz";
    double v = hz;
    if (hz >= 1e6) {
        v = hz / 1e6;
        unit = "MHz";
    } else if (hz >= 1e3) {
        v = hz / 1e3;
        unit = "kHz";
    }
    snprintf(out, size, "%.6f", v);
    // Trailing zeros (and a bare point) go.
    char *dot = strchr(out, '.');
    if (dot) {
        char *e = out + strlen(out) - 1;
        while (e > dot && *e == '0')
            *e-- = 0;
        if (e == dot)
            *e = 0;
    }
    int n = strlen(out);
    snprintf(out + n, size - n, " %s", unit);
    return out;
}

bool si5351_parse_freq(const char *s, uint32_t *hz) {
    while (*s == ' ')
        ++s;
    char *end;
    double v = strtod(s, &end);
    if (end == s || v < 0)
        return false;
    while (*end == ' ')
        ++end;
    double mul = 1;
    if (*end == 'k' || *end == 'K')
        mul = 1e3, ++end;
    else if (*end == 'M' || *end == 'm')
        mul = 1e6, ++end;
    else if (*end == 'G' || *end == 'g')
        mul = 1e9, ++end;
    if ((end[0] == 'H' || end[0] == 'h') && (end[1] == 'z' || end[1] == 'Z'))
        end += 2;
    while (*end == ' ')
        ++end;
    if (*end)
        return false;
    double f = v * mul;
    if (f > 4e9)
        return false;
    *hz = (uint32_t)llround(f);
    return true;
}
