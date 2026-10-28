/**
 * @file rtl2832.c
 * @brief The RTL2832U demodulator in SDR mode and its R820T/R828D tuner: see rtl2832.h.
 *
 * The RTL2832U is set up with vendor control requests: a register in one of
 * its blocks (USB, system, I2C), or a demodulator register in one of its
 * pages. The tuner sits on its I2C bus, behind a repeater that is switched
 * on while the tuner is talked to. The tuner mixes the antenna's signal down
 * to a low IF (about 2 MHz) and the RTL2832U's ADC and digital mixer bring it
 * to zero, then resample it to the rate asked for.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */

#include "rtl2832.h"

#include <stdio.h>
#include <string.h>

#include "pico/time.h"
#include "rtl_usb.h"

/// The RTL2832U's register blocks.
enum { BLK_DEMOD = 0, BLK_USB = 1, BLK_SYS = 2, BLK_I2C = 6 };
/// USB block registers.
enum { USB_SYSCTL = 0x2000, USB_EPA_CTL = 0x2148, USB_EPA_MAXPKT = 0x2158 };
/// System block registers.
enum { SYS_DEMOD_CTL = 0x3000, SYS_GPO = 0x3001, SYS_GPOE = 0x3003, SYS_GPD = 0x3004, SYS_DEMOD_CTL_1 = 0x300b };

#define R820T_ADDR 0x34   ///< the R820T's I2C address (8-bit form)
#define R828D_ADDR 0x74   ///< the R828D's
#define R82XX_ID   0x69   ///< what both answer at register 0
#define R828D_XTAL 16000000u  ///< an R828D's own crystal (not on the Blog V4)
#define DIRECT_BELOW 24000000u  ///< R820T dongles sample the antenna directly below this
#define V4_UPCONVERT 28800000u  ///< the Blog V4's upconverter adds this below it

// ---- Requests ---------------------------------------------------------------------------------------

/// Notes a failed request.
/// @param d the dongle
/// @param r the request's result
/// @param want the bytes it should have moved
/// @return true if it moved them
static bool ok(rtl_dev_t *d, int r, int want) {
    if (r != want)
        d->failed = true;
    return r == want;
}

/// Writes a register of a block, 1 or 2 bytes (high byte first).
/// @param d the dongle
/// @param block BLK_*
/// @param addr the register
/// @param val its value
/// @param len 1 or 2
/// @return false if it failed
static bool write_reg(rtl_dev_t *d, int block, uint16_t addr, uint16_t val, int len) {
    uint8_t b[2] = {(uint8_t)(len == 1 ? val : val >> 8), (uint8_t)val};
    return ok(d, rtl_usb_control(0x40, 0, addr, (uint16_t)(block << 8 | 0x10), b, (uint16_t)len), len);
}

/// Reads a register of a block (low byte first).
/// @param d the dongle
/// @param block BLK_*
/// @param addr the register
/// @param len 1 or 2
/// @return its value (0 if it failed)
static uint16_t read_reg(rtl_dev_t *d, int block, uint16_t addr, int len) {
    uint8_t b[2] = {0, 0};
    ok(d, rtl_usb_control(0xc0, 0, addr, (uint16_t)(block << 8), b, (uint16_t)len), len);
    return (uint16_t)(b[1] << 8 | b[0]);
}

/// Reads a demodulator register.
/// @param d the dongle
/// @param page its page
/// @param addr the register
/// @return its value
static uint8_t demod_read(rtl_dev_t *d, int page, uint8_t addr) {
    uint8_t b = 0;
    ok(d, rtl_usb_control(0xc0, 0, (uint16_t)(addr << 8 | 0x20), (uint16_t)page, &b, 1), 1);
    return b;
}

/// Writes a demodulator register, 1 or 2 bytes, then reads one back as the
/// chip wants (page 10, register 1).
/// @param d the dongle
/// @param page its page
/// @param addr the register
/// @param val its value
/// @param len 1 or 2
/// @return false if it failed
static bool demod_write(rtl_dev_t *d, int page, uint8_t addr, uint16_t val, int len) {
    uint8_t b[2] = {(uint8_t)(len == 1 ? val : val >> 8), (uint8_t)val};
    bool r = ok(d, rtl_usb_control(0x40, 0, (uint16_t)(addr << 8 | 0x20), (uint16_t)(0x10 | page), b, (uint16_t)len),
                len);
    demod_read(d, 0x0a, 0x01);
    return r;
}

/// Writes bytes to a chip on the I2C bus.
/// @param d the dongle
/// @param i2c its address
/// @param buf the bytes (the register first)
/// @param len how many
/// @return false if it failed
static bool i2c_write(rtl_dev_t *d, uint8_t i2c, const uint8_t *buf, int len) {
    return ok(d, rtl_usb_control(0x40, 0, i2c, BLK_I2C << 8 | 0x10, (void *)buf, (uint16_t)len), len);
}

/// Reads bytes from a chip on the I2C bus.
/// @param d the dongle
/// @param i2c its address
/// @param buf where they go
/// @param len how many
/// @return false if it failed
static bool i2c_read(rtl_dev_t *d, uint8_t i2c, uint8_t *buf, int len) {
    return ok(d, rtl_usb_control(0xc0, 0, i2c, BLK_I2C << 8, buf, (uint16_t)len), len);
}

/// Reads one register of a chip on the I2C bus, quietly (for probing).
/// @param d the dongle
/// @param i2c its address
/// @param reg the register
/// @return its value, or -1 if nothing answered
static int i2c_probe(rtl_dev_t *d, uint8_t i2c, uint8_t reg) {
    uint8_t v = 0;
    if (rtl_usb_control(0x40, 0, i2c, BLK_I2C << 8 | 0x10, &reg, 1) != 1)
        return -1;
    if (rtl_usb_control(0xc0, 0, i2c, BLK_I2C << 8, &v, 1) != 1)
        return -1;
    return v;
}

/// The I2C repeater to the tuner.
/// @param d the dongle
/// @param on through
static void repeater(rtl_dev_t *d, bool on) {
    demod_write(d, 1, 0x01, on ? 0x18 : 0x10, 1);
}

/// A GPIO as an output, set high or low.
/// @param d the dongle
/// @param gpio its number
/// @param on high
static void gpio_set(rtl_dev_t *d, int gpio, bool on) {
    uint8_t bit = (uint8_t)(1u << gpio);
    write_reg(d, BLK_SYS, SYS_GPD, read_reg(d, BLK_SYS, SYS_GPD, 1) & ~bit, 1);
    write_reg(d, BLK_SYS, SYS_GPOE, read_reg(d, BLK_SYS, SYS_GPOE, 1) | bit, 1);
    uint16_t v = read_reg(d, BLK_SYS, SYS_GPO, 1);
    write_reg(d, BLK_SYS, SYS_GPO, on ? (v | bit) : (v & ~bit), 1);
}

/// The crystal's frequency after the correction.
/// @param d the dongle
/// @param hz the nominal frequency
/// @return the corrected one
static uint32_t corrected(const rtl_dev_t *d, uint32_t hz) {
    return (uint32_t)((int64_t)hz + (int64_t)hz * d->ppm / 1000000);
}

// ---- The R820T / R828D tuner ----------------------------------------------------------------------------

#define R82_FIRST 5   ///< the first register written (the shadow copy starts here)
#define R82_COUNT 27  ///< registers 5 to 31

/// Registers 5 to 31 at the start.
static const uint8_t r82_init_regs[R82_COUNT] = {
    0x83, 0x30, 0x75, 0xc0, 0x40, 0xd6, 0x6c, 0xf5, 0x63, 0x75, 0x68, 0x6c, 0x83, 0x80,
    0x00, 0x0f, 0x00, 0xc0, 0x30, 0x48, 0xcc, 0x60, 0x00, 0x54, 0xae, 0x4a, 0xc0,
};

/// The tracking filter and input mux for each band of LO frequencies: from
/// MHz, the open-drain bit (register 0x17), the RF mux and polyphase filter
/// (0x1a) and the tracking filter's band (0x1b).
static const struct {
    uint16_t mhz;
    uint8_t open_d, mux, tf;
} r82_bands[] = {
    {0, 0x08, 0x02, 0xdf},   {50, 0x08, 0x02, 0xbe},  {55, 0x08, 0x02, 0x8b},  {60, 0x08, 0x02, 0x7b},
    {65, 0x08, 0x02, 0x69},  {70, 0x08, 0x02, 0x58},  {75, 0x00, 0x02, 0x44},  {90, 0x00, 0x02, 0x34},
    {110, 0x00, 0x02, 0x24}, {140, 0x00, 0x02, 0x14}, {180, 0x00, 0x02, 0x13}, {250, 0x00, 0x02, 0x11},
    {280, 0x00, 0x02, 0x00}, {310, 0x00, 0x41, 0x00}, {588, 0x00, 0x40, 0x00},
};

/// The LNA's and mixer's gain steps (tenths of a dB), measured by the
/// rtl-sdr project: each index adds its step to the one before.
static const int8_t lna_steps[16] = {0, 9, 13, 40, 38, 13, 31, 22, 26, 31, 26, 14, 19, 5, 35, 13};
static const int8_t mix_steps[16] = {0, 5, 10, 10, 19, 9, 10, 25, 17, 10, 8, 16, 13, 6, 3, -8};

/// The total gains those make, alternately stepping the LNA and the mixer.
static const int16_t gain_list[] = {0,   9,   14,  27,  37,  77,  87,  125, 144, 157, 166, 197, 207, 229, 254,
                                    280, 297, 328, 338, 364, 372, 386, 402, 421, 434, 439, 445, 480, 496};

/// Writes tuner registers (a few at a time, as the RTL2832U's I2C takes at
/// most 8 bytes), keeping the shadow copy.
/// @param d the dongle
/// @param reg the first
/// @param val their values
/// @param n how many
/// @return false if it failed
static bool r82_write(rtl_dev_t *d, uint8_t reg, const uint8_t *val, int n) {
    r82xx_t *t = &d->r82;
    for (int i = 0; i < n; ++i)
        if (reg + i >= R82_FIRST && reg + i < R82_FIRST + R82_COUNT)
            t->regs[reg + i - R82_FIRST] = val[i];
    while (n > 0) {
        int k = n > 7 ? 7 : n;
        uint8_t b[8];
        b[0] = reg;
        memcpy(b + 1, val, k);
        if (!i2c_write(d, t->i2c, b, k + 1))
            return false;
        reg += k, val += k, n -= k;
    }
    return true;
}

/// Writes one tuner register.
/// @param d the dongle
/// @param reg it
/// @param v its value
/// @return false if it failed
static bool r82_set(rtl_dev_t *d, uint8_t reg, uint8_t v) {
    return r82_write(d, reg, &v, 1);
}

/// Changes some bits of a tuner register (from the shadow copy).
/// @param d the dongle
/// @param reg it
/// @param v the new bits
/// @param mask which bits
/// @return false if it failed
static bool r82_mask(rtl_dev_t *d, uint8_t reg, uint8_t v, uint8_t mask) {
    uint8_t old = d->r82.regs[reg - R82_FIRST];
    return r82_set(d, reg, (uint8_t)((old & ~mask) | (v & mask)));
}

/// Reads the tuner's first registers (it always reads from 0, bits reversed).
/// @param d the dongle
/// @param val where they go
/// @param n how many
/// @return false if it failed
static bool r82_read(rtl_dev_t *d, uint8_t *val, int n) {
    uint8_t zero = 0;
    if (!i2c_write(d, d->r82.i2c, &zero, 1) || !i2c_read(d, d->r82.i2c, val, n))
        return false;
    for (int i = 0; i < n; ++i) {
        uint8_t b = val[i], r = 0;
        for (int k = 0; k < 8; ++k)
            r |= (uint8_t)(((b >> k) & 1) << (7 - k));
        val[i] = r;
    }
    return true;
}

/// The tracking filter and input for a LO frequency.
/// @param d the dongle
/// @param lo the LO's frequency (Hz)
/// @return false if it failed
static bool r82_set_mux(rtl_dev_t *d, uint32_t lo) {
    uint32_t mhz = lo / 1000000;
    unsigned i = 0;
    while (i + 1 < sizeof r82_bands / sizeof *r82_bands && mhz >= r82_bands[i + 1].mhz)
        ++i;
    return r82_mask(d, 0x17, r82_bands[i].open_d, 0x08) && r82_mask(d, 0x1a, r82_bands[i].mux, 0xc3) &&
           r82_set(d, 0x1b, r82_bands[i].tf) && r82_mask(d, 0x10, 0x00, 0x0b) && r82_mask(d, 0x08, 0x00, 0x3f) &&
           r82_mask(d, 0x09, 0x00, 0x3f);
}

/// Sets the PLL (the LO) and waits for it to lock.
/// @param d the dongle
/// @param lo the frequency (Hz)
/// @return false if a request failed or the frequency is out of range
static bool r82_set_pll(rtl_dev_t *d, uint32_t lo) {
    r82xx_t *t = &d->r82;
    const uint32_t vco_min = 1770000, vco_max = 2 * vco_min;  // kHz
    uint32_t lo_khz = (lo + 500) / 1000, ref = t->xtal, ref_khz = (t->xtal + 500) / 1000;
    t->locked = false;
    // No reference divider; PLL autotune 128 kHz; the VCO's current at its most.
    if (!r82_mask(d, 0x10, 0x00, 0x10) || !r82_mask(d, 0x1a, 0x00, 0x0c) || !r82_mask(d, 0x12, 0x06, 0xff))
        return false;
    // The mixer's divider: the VCO between 1.77 and 3.54 GHz.
    unsigned mix_div = 2, div_num = 0;
    for (; mix_div <= 64; mix_div <<= 1)
        if (lo_khz * mix_div >= vco_min && lo_khz * mix_div < vco_max)
            break;
    if (mix_div > 64)
        return false;
    for (unsigned b = mix_div; b > 2; b >>= 1)
        ++div_num;
    uint8_t data[5];
    if (!r82_read(d, data, 5))
        return false;
    unsigned power_ref = t->r828d ? 1 : 2;
    unsigned fine = (data[4] & 0x30) >> 4;
    if (fine > power_ref)
        --div_num;
    else if (fine < power_ref)
        ++div_num;
    if (!r82_mask(d, 0x10, (uint8_t)(div_num << 5), 0xe0))
        return false;
    uint64_t vco = (uint64_t)lo * mix_div;
    uint32_t nint = (uint32_t)(vco / (2ull * ref));
    uint32_t fra = (uint32_t)((vco - 2ull * ref * nint) / 1000);  // kHz the sigma-delta adds
    if (nint < 13 || nint > 128 / power_ref - 1)
        return false;
    uint32_t ni = (nint - 13) / 4, si = nint - 4 * ni - 13;
    if (!r82_set(d, 0x14, (uint8_t)(ni + (si << 6))) || !r82_mask(d, 0x12, fra ? 0x00 : 0x08, 0x08))
        return false;
    // The sigma-delta modulator's fraction, a bit at a time.
    uint32_t n_sdm = 2, sdm = 0;
    while (fra > 1) {
        if (fra > 2 * ref_khz / n_sdm) {
            sdm += 32768 / (n_sdm / 2);
            fra -= 2 * ref_khz / n_sdm;
            if (n_sdm >= 0x8000)
                break;
        }
        n_sdm <<= 1;
    }
    if (!r82_set(d, 0x16, (uint8_t)(sdm >> 8)) || !r82_set(d, 0x15, (uint8_t)sdm))
        return false;
    for (int i = 0; i < 2; ++i) {
        if (!r82_read(d, data, 3))
            return false;
        if (data[2] & 0x40)
            break;
        if (!i && !r82_mask(d, 0x12, 0x06, 0xff))  // not locked: the VCO's current again
            return false;
    }
    if (!(data[2] & 0x40))
        return true;  // not locked: t->locked stays false
    t->locked = true;
    return r82_mask(d, 0x1a, 0x08, 0x08);  // PLL autotune 8 kHz
}

/// The tuner's set-up: registers, filter calibration, LNA and mixer for
/// digital reception.
/// @param d the dongle
/// @return false if it failed
static bool r82_init(rtl_dev_t *d) {
    r82xx_t *t = &d->r82;
    t->input = 0;
    if (!r82_write(d, R82_FIRST, r82_init_regs, R82_COUNT))
        return false;
    // The standard (under 6 MHz wide, 3.57 MHz IF), from the start values.
    memcpy(t->regs, r82_init_regs, R82_COUNT);
    if (!r82_mask(d, 0x0c, 0x00, 0x0f) || !r82_mask(d, 0x13, 49, 0x3f) || !r82_mask(d, 0x1d, 0x00, 0x38))
        return false;
    t->if_hz = 3570000;
    // Calibrate the IF filter (at 56 MHz), twice if the first is no good.
    uint8_t cal = 0;
    for (int i = 0; i < 2; ++i) {
        if (!r82_mask(d, 0x0b, 0x6b, 0x60) || !r82_mask(d, 0x0f, 0x04, 0x04) || !r82_mask(d, 0x10, 0x00, 0x03) ||
            !r82_set_pll(d, 56000000) || !t->locked)
            return false;
        uint8_t data[5];
        if (!r82_mask(d, 0x0b, 0x10, 0x10) || !r82_mask(d, 0x0b, 0x00, 0x10) || !r82_mask(d, 0x0f, 0x00, 0x04) ||
            !r82_read(d, data, 5))
            return false;
        cal = data[4] & 0x0f;
        if (cal && cal != 0x0f)
            break;
    }
    if (cal == 0x0f)
        cal = 0;
    bool r = r82_mask(d, 0x0a, (uint8_t)(0x10 | cal), 0x1f) && r82_mask(d, 0x0b, 0x6b, 0xef) &&
             r82_mask(d, 0x07, 0x00, 0x80) && r82_mask(d, 0x06, 0x30, 0x30) && r82_mask(d, 0x1e, 0x60, 0x60) &&
             r82_mask(d, 0x05, 0x80, 0x80) && r82_mask(d, 0x1f, 0x00, 0x80) && r82_mask(d, 0x0f, 0x00, 0x80) &&
             r82_mask(d, 0x19, 0x60, 0x60);
    // The LNA and mixer for digital TV (the SDR's use), the air input.
    r = r && r82_mask(d, 0x1d, 0xe5, 0xc7) && r82_mask(d, 0x1c, 0x24, 0xf8) && r82_set(d, 0x0d, 0x53) &&
        r82_set(d, 0x0e, 0x75) && r82_mask(d, 0x05, 0x00, 0x60) && r82_mask(d, 0x06, 0x00, 0x08) &&
        r82_mask(d, 0x11, 0x38, 0x38) && r82_mask(d, 0x17, 0x20, 0x30) && r82_mask(d, 0x0a, 0x40, 0x60) &&
        r82_mask(d, 0x1d, 0x00, 0x38) && r82_mask(d, 0x1c, 0x00, 0x04) && r82_mask(d, 0x06, 0x00, 0x40) &&
        r82_mask(d, 0x1a, 0x30, 0x30) && r82_mask(d, 0x1d, 0x18, 0x38) && r82_mask(d, 0x1c, 0x24, 0x04) &&
        r82_mask(d, 0x1e, 14, 0x1f) && r82_mask(d, 0x1a, 0x20, 0x30);
    return r;
}

/// The IF filter for a bandwidth: the narrowest (350 kHz) for the low rates.
/// Sets t->if_hz to the IF it centres on.
/// @param d the dongle
/// @param bw the bandwidth wanted (Hz)
/// @return false if it failed
static bool r82_set_bw(rtl_dev_t *d, uint32_t bw) {
    static const uint32_t lpf[] = {1700000, 1600000, 1550000, 1450000, 1200000, 900000, 700000, 550000, 450000, 350000};
    const uint32_t hp1 = 350000, hp2 = 380000;
    r82xx_t *t = &d->r82;
    uint8_t r0a = 0x00, r0b = 0x80;
    uint32_t real = 0;
    t->if_hz = 2300000;
    if (bw > lpf[0] + hp1) {
        bw -= hp2;
        t->if_hz += hp2;
        real += hp2;
    } else {
        r0b |= 0x20;
    }
    if (bw > lpf[0]) {
        bw -= hp1;
        t->if_hz += hp1;
        real += hp1;
    } else {
        r0b |= 0x40;
    }
    unsigned i = 0;
    while (i < sizeof lpf / sizeof *lpf && bw <= lpf[i])
        ++i;
    if (i)
        --i;
    r0b |= (uint8_t)(15 - i);
    real += lpf[i];
    t->if_hz -= real / 2;
    return r82_mask(d, 0x0a, r0a, 0x10) && r82_mask(d, 0x0b, r0b, 0xef);
}

/// Tunes the tuner (LO = frequency + IF), switching the Blog V4's inputs.
/// @param d the dongle
/// @param hz the frequency
/// @return false if it failed or the PLL didn't lock
static bool r82_set_freq(rtl_dev_t *d, uint32_t hz) {
    r82xx_t *t = &d->r82;
    uint32_t rf = d->blog_v4 && hz < V4_UPCONVERT ? hz + V4_UPCONVERT : hz;
    uint32_t lo = rf + t->if_hz;
    if (!r82_set_mux(d, lo) || !r82_mask(d, 0x0c, 0x08, 0x9f) || !r82_set_pll(d, lo) || !t->locked)
        return false;
    if (d->blog_v4) {
        // Its notch filters (FM, DAB...) on except when tuned inside them.
        bool in_notch = hz <= 2200000 || (hz >= 85000000 && hz <= 112000000) || (hz >= 172000000 && hz <= 242000000);
        if (!r82_mask(d, 0x17, in_notch ? 0x00 : 0x08, 0x08))
            return false;
        enum { HF = 1, VHF, UHF };
        int band = hz <= V4_UPCONVERT ? HF : hz < 250000000 ? VHF : UHF;
        if (band == HF && (!r82_mask(d, 0x1a, 0x40, 0xc3) || !r82_set(d, 0x1b, 0x00)))
            return false;  // the tracking filter bypassed for the upconverter
        if (band != t->input) {
            t->input = band;
            if (!r82_mask(d, 0x06, band == HF ? 0x08 : 0x00, 0x08))  // cable 2: HF
                return false;
            gpio_set(d, 5, band != HF);  // the upconverter's switch
            if (!r82_mask(d, 0x05, band == VHF ? 0x40 : 0x00, 0x40) ||  // cable 1: VHF
                !r82_mask(d, 0x05, band == UHF ? 0x00 : 0x20, 0x20))    // air: UHF
                return false;
        }
    } else if (t->r828d) {
        int in = hz > 345000000 ? 0x00 : 0x60;  // air in above 345 MHz, else cable 1
        if (in != t->input) {
            t->input = in;
            if (!r82_mask(d, 0x05, (uint8_t)in, 0x60))
                return false;
        }
    }
    return true;
}

/// The tuner's gain: manual (LNA and mixer stepped up to it) or its AGC.
/// @param d the dongle
/// @param tenth_db the gain, or RTL_GAIN_AUTO
/// @return false if it failed
static bool r82_set_gain(rtl_dev_t *d, int tenth_db) {
    if (tenth_db == RTL_GAIN_AUTO)
        return r82_mask(d, 0x05, 0x00, 0x10) && r82_mask(d, 0x07, 0x10, 0x10) && r82_mask(d, 0x0c, 0x0b, 0x9f);
    uint8_t data[4];
    if (!r82_mask(d, 0x05, 0x10, 0x10) || !r82_mask(d, 0x07, 0x00, 0x10) || !r82_read(d, data, 4) ||
        !r82_mask(d, 0x0c, 0x08, 0x9f))
        return false;
    int total = 0;
    unsigned lna = 0, mix = 0;
    for (int i = 0; i < 15; ++i) {
        if (total >= tenth_db)
            break;
        total += lna_steps[++lna];
        if (total >= tenth_db)
            break;
        total += mix_steps[++mix];
    }
    return r82_mask(d, 0x05, (uint8_t)lna, 0x0f) && r82_mask(d, 0x07, (uint8_t)mix, 0x0f);
}

/// The tuner to sleep.
/// @param d the dongle
static void r82_standby(rtl_dev_t *d) {
    static const uint8_t regs[][2] = {{0x06, 0xb1}, {0x05, 0xa0}, {0x07, 0x3a}, {0x08, 0x40}, {0x09, 0xc0}, {0x0a, 0x36},
                                      {0x0c, 0x35}, {0x0f, 0x68}, {0x11, 0x03}, {0x17, 0xf4}, {0x19, 0x0c}};
    for (unsigned i = 0; i < sizeof regs / sizeof *regs; ++i)
        r82_set(d, regs[i][0], regs[i][1]);
}

// ---- The demodulator --------------------------------------------------------------------------------

/// The RTL2832U's own low-pass filter before its resampler (the coefficients
/// the Windows driver uses for FM and DAB): 8 of 8 bits, then 8 of 12.
static const int16_t fir_coeffs[16] = {-54, -36, -41, -40, -32, -14, 14, 53, 101, 156, 215, 273, 327, 372, 404, 421};

/// Writes the low-pass filter's coefficients (registers 0x1c-0x2f of page 1).
/// @param d the dongle
static void set_fir(rtl_dev_t *d) {
    uint8_t b[20];
    for (int i = 0; i < 8; ++i)
        b[i] = (uint8_t)fir_coeffs[i];
    for (int i = 0; i < 8; i += 2) {
        int v0 = fir_coeffs[8 + i], v1 = fir_coeffs[8 + i + 1];
        b[8 + i * 3 / 2] = (uint8_t)(v0 >> 4);
        b[8 + i * 3 / 2 + 1] = (uint8_t)(v0 << 4 | ((v1 >> 8) & 0x0f));
        b[8 + i * 3 / 2 + 2] = (uint8_t)v1;
    }
    for (int i = 0; i < 20; ++i)
        demod_write(d, 1, (uint8_t)(0x1c + i), b[i], 1);
}

/// The digital mixer's frequency: the IF, or the antenna's frequency when
/// sampling directly.
/// @param d the dongle
/// @param hz it
static void set_if(rtl_dev_t *d, uint32_t hz) {
    int32_t v = -(int32_t)(((int64_t)hz << 22) / corrected(d, RTL_XTAL_HZ));
    demod_write(d, 1, 0x19, (v >> 16) & 0x3f, 1);
    demod_write(d, 1, 0x1a, (v >> 8) & 0xff, 1);
    demod_write(d, 1, 0x1b, v & 0xff, 1);
}

/// The sample clock's correction.
/// @param d the dongle
static void set_sample_correction(rtl_dev_t *d) {
    int16_t offs = (int16_t)(-d->ppm * (1 << 24) / 1000000);
    demod_write(d, 1, 0x3f, offs & 0xff, 1);
    demod_write(d, 1, 0x3e, (offs >> 8) & 0x3f, 1);
}

/// Sampling the antenna directly (the Q branch's ADC), or through the tuner.
/// @param d the dongle
/// @param on direct
static void set_direct(rtl_dev_t *d, bool on) {
    if (on == d->direct)
        return;
    if (on) {
        repeater(d, true);
        r82_standby(d);
        repeater(d, false);
        demod_write(d, 1, 0xb1, 0x1a, 1);  // no zero-IF
        demod_write(d, 1, 0x15, 0x00, 1);  // no spectrum inversion
        demod_write(d, 0, 0x08, 0x4d, 1);  // the I ADC only
        demod_write(d, 0, 0x06, 0x90, 1);  // I and Q swapped: the Q input
    } else {
        repeater(d, true);
        r82_init(d);
        r82_set_bw(d, d->rate);
        r82_set_gain(d, d->gain);
        repeater(d, false);
        set_if(d, d->r82.if_hz);
        demod_write(d, 1, 0x15, 0x01, 1);  // spectrum inversion
        demod_write(d, 0, 0x06, 0x80, 1);
    }
    d->direct = on;
}

/// Reads a USB string descriptor as ASCII.
/// @param idx its index (0: none)
/// @param out where it goes
/// @param n its room
static void usb_string(uint8_t idx, char *out, int n) {
    out[0] = 0;
    uint8_t b[64];
    if (!idx || rtl_usb_control(0x80, 6, (uint16_t)(0x0300 | idx), 0x0409, b, sizeof b) < 2)
        return;
    int len = b[0] < sizeof b ? b[0] : (int)sizeof b, k = 0;
    for (int i = 2; i + 1 < len && k < n - 1; i += 2)
        out[k++] = b[i + 1] ? '?' : (char)b[i];
    out[k] = 0;
}

bool rtl_open(rtl_dev_t *d, char *err, int errlen) {
    memset(d, 0, sizeof *d);
    d->gain = RTL_GAIN_AUTO;
    if (!rtl_usb_present()) {
        snprintf(err, errlen, "no RTL-SDR dongle plugged in");
        return false;
    }
    if (!write_reg(d, BLK_USB, USB_SYSCTL, 0x09, 1)) {
        snprintf(err, errlen, "the dongle doesn't answer");
        return false;
    }
    uint8_t dd[18];
    if (rtl_usb_control(0x80, 6, 0x0100, 0, dd, sizeof dd) == sizeof dd) {
        usb_string(dd[14], d->maker, sizeof d->maker);
        usb_string(dd[15], d->product, sizeof d->product);
    }
    d->blog_v4 = !strcmp(d->maker, "RTLSDRBlog") && !strcmp(d->product, "Blog V4");
    // USB: the endpoint's packet size as it was enumerated (64 at full speed).
    uint16_t mps = rtl_usb_packet_size();
    write_reg(d, BLK_USB, USB_EPA_MAXPKT, (uint16_t)((mps & 0xff) << 8 | mps >> 8), 2);
    write_reg(d, BLK_USB, USB_EPA_CTL, 0x1002, 2);  // FIFO held in reset
    // The demodulator on, reset, in SDR mode with no AGC of its own.
    write_reg(d, BLK_SYS, SYS_DEMOD_CTL_1, 0x22, 1);
    write_reg(d, BLK_SYS, SYS_DEMOD_CTL, 0xe8, 1);
    demod_write(d, 1, 0x01, 0x14, 1);
    demod_write(d, 1, 0x01, 0x10, 1);
    demod_write(d, 1, 0x15, 0x00, 1);
    demod_write(d, 1, 0x16, 0x0000, 2);
    for (int i = 0; i < 6; ++i)
        demod_write(d, 1, (uint8_t)(0x16 + i), 0x00, 1);
    set_fir(d);
    demod_write(d, 0, 0x19, 0x05, 1);  // SDR mode, no digital AGC
    demod_write(d, 1, 0x93, 0xf0, 1);
    demod_write(d, 1, 0x94, 0x0f, 1);
    demod_write(d, 1, 0x11, 0x00, 1);
    demod_write(d, 1, 0x04, 0x00, 1);  // no RF or IF AGC loop
    demod_write(d, 0, 0x61, 0x60, 1);  // no PID filter
    demod_write(d, 0, 0x06, 0x80, 1);
    demod_write(d, 1, 0xb1, 0x1b, 1);  // zero-IF, DC and IQ correction
    demod_write(d, 0, 0x0d, 0x83, 1);  // no 4.096 MHz clock out
    if (d->failed) {
        snprintf(err, errlen, "the dongle didn't take its settings");
        return false;
    }
    // Which tuner.
    repeater(d, true);
    if (i2c_probe(d, R820T_ADDR, 0) == R82XX_ID)
        d->tuner = RTL_TUNER_R820T;
    else if (i2c_probe(d, R828D_ADDR, 0) == R82XX_ID)
        d->tuner = RTL_TUNER_R828D;
    else if (i2c_probe(d, 0xc8, 0x02) == 0x40)
        d->tuner = RTL_TUNER_E4000;
    else if (i2c_probe(d, 0xc6, 0x00) == 0xa3)
        d->tuner = RTL_TUNER_FC0013;
    else if (i2c_probe(d, 0xc6, 0x00) == 0xa1)
        d->tuner = RTL_TUNER_FC0012;
    d->failed = false;  // the probes of chips that aren't there
    if (d->tuner != RTL_TUNER_R820T && d->tuner != RTL_TUNER_R828D) {
        repeater(d, false);
        snprintf(err, errlen, d->tuner == RTL_TUNER_NONE ? "no tuner found" : "its %s tuner isn't supported",
                 rtl_tuner_name(d->tuner));
        return false;
    }
    r82xx_t *t = &d->r82;
    t->r828d = d->tuner == RTL_TUNER_R828D;
    t->i2c = t->r828d ? R828D_ADDR : R820T_ADDR;
    t->xtal = corrected(d, t->r828d && !d->blog_v4 ? R828D_XTAL : RTL_XTAL_HZ);
    // Low IF: no zero-IF, the I ADC only, spectrum inverted.
    demod_write(d, 1, 0xb1, 0x1a, 1);
    demod_write(d, 0, 0x08, 0x4d, 1);
    set_if(d, 3570000);
    demod_write(d, 1, 0x15, 0x01, 1);
    bool r = r82_init(d) && r82_set_gain(d, RTL_GAIN_AUTO);
    repeater(d, false);
    if (!r || d->failed) {
        snprintf(err, errlen, "the %s tuner didn't start", rtl_tuner_name(d->tuner));
        return false;
    }
    return true;
}

void rtl_close(rtl_dev_t *d) {
    if (!rtl_usb_present())
        return;
    if (d->tuner == RTL_TUNER_R820T || d->tuner == RTL_TUNER_R828D) {
        repeater(d, true);
        r82_standby(d);
        repeater(d, false);
    }
    if (d->bias_tee)
        gpio_set(d, 0, false);
    write_reg(d, BLK_SYS, SYS_DEMOD_CTL, 0x20, 1);  // demodulator and ADCs off
}

bool rtl_set_rate(rtl_dev_t *d, uint32_t hz) {
    if (hz <= 225000 || hz > 3200000 || (hz > 300000 && hz <= 900000))
        return false;
    d->failed = false;
    uint32_t xtal = corrected(d, RTL_XTAL_HZ);
    uint32_t ratio = (uint32_t)(((uint64_t)xtal << 22) / hz) & 0x0ffffffc;
    uint32_t real = ratio | ((ratio & 0x08000000) << 1);
    d->rate = (uint32_t)(((uint64_t)xtal << 22) / real);
    if (!d->direct) {
        repeater(d, true);
        r82_set_bw(d, d->rate);
        repeater(d, false);
        set_if(d, d->r82.if_hz);
    }
    demod_write(d, 1, 0x9f, (uint16_t)(ratio >> 16), 2);
    demod_write(d, 1, 0xa1, (uint16_t)ratio, 2);
    set_sample_correction(d);
    demod_write(d, 1, 0x01, 0x14, 1);  // soft reset
    demod_write(d, 1, 0x01, 0x10, 1);
    if (d->freq)
        rtl_set_freq(d, d->freq);
    return !d->failed;
}

bool rtl_set_freq(rtl_dev_t *d, uint32_t hz) {
    d->failed = false;
    set_direct(d, d->tuner == RTL_TUNER_R820T && !d->blog_v4 && hz < DIRECT_BELOW);
    bool r;
    if (d->direct) {
        set_if(d, hz);
        r = true;
    } else {
        repeater(d, true);
        r = r82_set_freq(d, hz);
        repeater(d, false);
    }
    d->freq = hz;
    return r && !d->failed;
}

bool rtl_set_gain(rtl_dev_t *d, int tenth_db) {
    d->failed = false;
    d->gain = tenth_db;
    if (d->direct)
        return true;
    repeater(d, true);
    bool r = r82_set_gain(d, tenth_db);
    repeater(d, false);
    return r && !d->failed;
}

bool rtl_set_ppm(rtl_dev_t *d, int ppm) {
    if (ppm < -200 || ppm > 200)
        return false;
    d->ppm = ppm;
    d->r82.xtal = corrected(d, d->r82.r828d && !d->blog_v4 ? R828D_XTAL : RTL_XTAL_HZ);
    return d->rate ? rtl_set_rate(d, d->rate) : true;  // the rate, IF and tuning again
}

bool rtl_set_bias_tee(rtl_dev_t *d, bool on) {
    d->failed = false;
    gpio_set(d, 0, on);
    d->bias_tee = on;
    return !d->failed;
}

bool rtl_stream(rtl_dev_t *d, bool on) {
    d->failed = false;
    write_reg(d, BLK_USB, USB_EPA_CTL, 0x1002, 2);
    if (on)
        write_reg(d, BLK_USB, USB_EPA_CTL, 0x0000, 2);
    return !d->failed;
}

const int16_t *rtl_gains(int *n) {
    *n = (int)(sizeof gain_list / sizeof *gain_list);
    return gain_list;
}

const char *rtl_tuner_name(rtl_tuner_t t) {
    switch (t) {
    case RTL_TUNER_R820T: return "R820T";
    case RTL_TUNER_R828D: return "R828D";
    case RTL_TUNER_E4000: return "E4000";
    case RTL_TUNER_FC0012: return "FC0012";
    case RTL_TUNER_FC0013: return "FC0013";
    default: return "unknown";
    }
}
