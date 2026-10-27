/**
 * @file si5351.h
 * @brief A Si5351A (3 outputs) clock generator: frequency planning, the register image and writing it over I2C.
 *
 * Plain C, so the frequency maths can be tested on a PC. The chip is reached
 * through two callbacks (si5351_bus_t); si5351_pico.c gives them the
 * RP2350's I2C, and the tests a simulated chip.
 *
 * How a frequency is made (Silicon Labs AN619):
 *  - a PLL multiplies the crystal up to a VCO frequency of 600-900 MHz:
 *    fVCO = fXTAL * (a + b/c), a 15-90, c up to 1048575;
 *  - a multisynth divides that down: fMS = fVCO / (a + b/c), 8-2048, or
 *    exactly 4 or 6 (integer mode; 4 needs DIVBY4);
 *  - an R divider (1-128, a power of two) divides again, for outputs below
 *    500 kHz.
 * Two PLLs (A and B) are shared by the three outputs. An output that has a
 * PLL to itself gets an even integer multisynth (the least jitter) and an
 * exact fractional PLL; outputs sharing a PLL take its VCO as the first of
 * them (the lowest numbered) set it, through a fractional multisynth.
 *
 * Phase offsets (CLKx_PHOFF) are in quarter VCO periods, up to 127, and need
 * an integer multisynth: 90 degrees is an offset equal to the divider. The
 * quadrature mode puts CLK1 on CLK0's PLL and frequency, 90 degrees behind
 * it, with an even divider of 126 or less (outputs of 4.8-112.5 MHz).
 *
 * Spread spectrum is left off: it works only on PLL A, its parameters are
 * tied to that PLL's settings, and a clock generator for radio work wants a
 * clean carrier.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define SI5351_OUTPUTS   3          ///< CLK0-CLK2 (the 10-pin Si5351A)
#define SI5351_ADDR      0x60       ///< its I2C address
#define SI5351_MIN_HZ    2500u      ///< the lowest output frequency offered
#define SI5351_MAX_HZ    200000000u ///< the highest
#define SI5351_VCO_MIN   600000000.0  ///< the PLLs' range, Hz
#define SI5351_VCO_MAX   900000000.0  ///< (upper end)
#define SI5351_FRAC_MAX  1048575u   ///< the largest c of a + b/c (20 bits)
#define SI5351_REGS      188        ///< the register image covers 0-187

/// Status register (0) bits.
enum {
    SI5351_SYS_INIT = 0x80,  ///< the chip is still starting up
    SI5351_LOL_B = 0x40,     ///< PLL B has lost lock
    SI5351_LOL_A = 0x20,     ///< PLL A has lost lock
    SI5351_LOS = 0x10,       ///< the CLKIN input is lost (the A has no CLKIN: ignore)
};

/// One output's settings.
typedef struct {
    uint32_t freq_hz;  ///< its frequency (SI5351_MIN_HZ-SI5351_MAX_HZ)
    bool enabled;      ///< driven (else powered down)
    uint8_t drive;     ///< output current: 0 2 mA, 1 4 mA, 2 6 mA, 3 8 mA
    bool invert;       ///< inverted
    uint8_t pll;       ///< 0 PLL A, 1 PLL B
    uint16_t phase;    ///< phase offset in degrees (0-359)
} si5351_out_t;

/// The whole setup: the crystal and the three outputs.
typedef struct {
    uint32_t xtal_hz;  ///< 25 or 27 MHz
    int32_t cal_ppb;   ///< the crystal's error in parts per billion (+: runs fast)
    uint8_t load_pf;   ///< crystal load capacitance: 6, 8 or 10 pF
    bool iq;           ///< quadrature: CLK1 = CLK0's frequency, 90 degrees behind
    si5351_out_t out[SI5351_OUTPUTS];  ///< CLK0-CLK2
} si5351_config_t;

/// A ratio a + b/c.
typedef struct {
    uint32_t a;  ///< the whole part
    uint32_t b;  ///< the fraction's numerator
    uint32_t c;  ///< and denominator (1 for an integer)
} si5351_ratio_t;

/// What si5351_plan worked out for an output.
typedef struct {
    si5351_ratio_t ms;  ///< its multisynth divider
    uint8_t r_log2;     ///< R divider: 1 << r_log2
    bool integer;       ///< the multisynth is an even integer (MSx_INT)
    bool divby4;        ///< divide by 4 (above 150 MHz)
    uint8_t pll;        ///< the PLL used (quadrature may move CLK1)
    uint8_t phoff;      ///< CLKx_PHOFF, quarter VCO periods
    double actual_hz;   ///< the frequency it really makes
    double phase_deg;   ///< the phase offset it really makes
    const char *error;  ///< why it can't be made (then it is left off), or NULL
} si5351_out_plan_t;

/// A plan: the PLLs and the outputs' dividers.
typedef struct {
    bool pll_used[2];          ///< a PLL feeds an enabled output
    si5351_ratio_t pll[2];     ///< PLL A and B's multipliers
    double vco_hz[2];          ///< and their VCO frequencies
    double xtal_hz;            ///< the crystal's frequency after calibration
    si5351_out_plan_t out[SI5351_OUTPUTS];  ///< CLK0-CLK2
} si5351_plan_t;

/// The I2C bus the chip is on.
typedef struct {
    /// Writes n registers from reg on; false if the chip didn't answer.
    bool (*write)(void *ctx, uint8_t reg, const uint8_t *data, int n);
    /// Reads n registers from reg on; false if the chip didn't answer.
    bool (*read)(void *ctx, uint8_t reg, uint8_t *data, int n);
    void *ctx;  ///< passed to both
} si5351_bus_t;

/// A chip: its bus, and the registers last written (so only changes are sent).
typedef struct {
    si5351_bus_t bus;          ///< how to reach it
    bool written;              ///< shadow holds what the chip has
    uint8_t shadow[SI5351_REGS];  ///< the registers last written
} si5351_dev_t;

/// The settings at power-up: 25 MHz crystal, 10 pF, CLK0 10 MHz (on), CLK1
/// 7.074 MHz and CLK2 14.074 MHz (off), CLK0 on PLL A, the others on B.
/// @param cfg filled in
void si5351_defaults(si5351_config_t *cfg);

/// The best a/b with b <= max_den for a value x in [0, 1) (continued fractions,
/// with the last semiconvergent checked).
/// @param x the value
/// @param max_den the largest denominator allowed
/// @param num set to the numerator
/// @param den set to the denominator (at least 1)
void si5351_best_ratio(double x, uint32_t max_den, uint32_t *num, uint32_t *den);

/// Works out the PLLs and dividers for a setup.
/// @param cfg the setup
/// @param plan filled in (an output that can't be made has its error set)
/// @return true if every enabled output can be made
bool si5351_plan(const si5351_config_t *cfg, si5351_plan_t *plan);

/// The register image for a plan: registers 0-187 as they should be (those
/// the driver doesn't use are 0).
/// @param cfg the setup
/// @param plan its plan
/// @param regs filled in
void si5351_registers(const si5351_config_t *cfg, const si5351_plan_t *plan, uint8_t regs[SI5351_REGS]);

/// A ratio's P1, P2, P3 parameters (AN619): P1 = 128a + floor(128b/c) - 512,
/// P2 = 128b - c floor(128b/c), P3 = c.
/// @param r the ratio
/// @param p1 set to P1
/// @param p2 set to P2
/// @param p3 set to P3
void si5351_params(si5351_ratio_t r, uint32_t *p1, uint32_t *p2, uint32_t *p3);

/// Starts a chip (nothing is written yet).
/// @param dev the chip
/// @param bus its bus
void si5351_init(si5351_dev_t *dev, const si5351_bus_t *bus);

/// Whether the chip answers.
/// @param dev the chip
/// @param status set to its status register (SI5351_SYS_INIT and the like), or NULL
/// @return it answered
bool si5351_probe(si5351_dev_t *dev, uint8_t *status);

/// Sets the chip up for a setup: plans it, then writes the registers that
/// differ from what the chip has. The first time (or after si5351_init) the
/// outputs are switched off, everything written, the PLLs reset and the
/// outputs switched on. Later, changes to only a PLL's fraction (fine
/// tuning) go straight in without a glitch; changed dividers, phases or
/// output settings reset the PLLs concerned (which also lines up phases).
/// @param dev the chip
/// @param cfg the setup
/// @param plan set to the plan, or NULL
/// @return false if the chip didn't answer
bool si5351_apply(si5351_dev_t *dev, const si5351_config_t *cfg, si5351_plan_t *plan);

/// Reads the registers the driver owns back from the chip, so the next
/// si5351_apply writes only what differs (outputs already running are left
/// alone).
/// @param dev the chip
/// @return false if the chip didn't answer
bool si5351_read_back(si5351_dev_t *dev);

/// Switches every output off (the output enable register and the drivers).
/// @param dev the chip
/// @return false if the chip didn't answer
bool si5351_all_off(si5351_dev_t *dev);

/// Forgets what the chip has, so the next si5351_apply writes everything.
/// @param dev the chip
void si5351_invalidate(si5351_dev_t *dev);

/// A frequency as text with its unit: "10 MHz", "7.074 MHz", "2.5 kHz".
/// @param hz the frequency
/// @param out the text
/// @param size room in out
/// @return out
char *si5351_freq_text(double hz, char *out, int size);

/// Parses a frequency: a number with an optional unit, k (kHz), M (MHz) or
/// Hz, e.g. "10M", "7.074 MHz", "14074k", "2500".
/// @param s the text
/// @param hz set to the frequency, rounded to 1 Hz
/// @return it parsed (whether in range is up to the caller)
bool si5351_parse_freq(const char *s, uint32_t *hz);
