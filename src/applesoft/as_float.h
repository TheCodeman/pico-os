/**
 * @file as_float.h
 * @brief Applesoft's floating point: 40-bit numbers (8-bit exponent, 32-bit mantissa) in a "floating accumulator" that carries an extra rounding byte.
 *
 * Every operation gives bit for bit the result the Apple's ROM does, quirks
 * included, so programs print the same digits and RND gives the same
 * sequence. The tests check that against the ROM itself.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

/// An Applesoft floating-point number, unpacked: sign, exponent and mantissa.
typedef struct {
    uint8_t exp;    ///< 0: the number is zero; else value = 0.mant * 2^(exp - 128)
    uint32_t mant;  ///< bit 31 set when normalised
    uint8_t ext;    ///< rounding byte, the bits below mant
    bool neg;  ///< negative
} as_fp;

/// A number as stored in memory: exponent, then the mantissa with its top bit
/// replaced by the sign.
typedef uint8_t as_packed[5];

/// Errors (Applesoft error codes) raise through this, which must not return.
extern void (*as_fp_error)(int code);
/// Applesoft error code: ?OVERFLOW ERROR.
#define AS_ERR_OVERFLOW  69
/// Applesoft error code: ?ILLEGAL QUANTITY ERROR.
#define AS_ERR_ILLQTY    53
/// Applesoft error code: ?DIVISION BY ZERO ERROR.
#define AS_ERR_DIVZERO   133

/// Unpacks a 5-byte Applesoft number from memory at `m`.
as_fp fp_unpack(const uint8_t *m);
/// Rounds f (in place, as the ROM does) and stores it.
void fp_pack(as_fp *f, uint8_t *m);
/// Rounds f in place: the rounding byte is folded into the mantissa.
void fp_round(as_fp *f);

/// Converts a signed 16-bit integer to a number.
as_fp fp_from_int16(int v);
as_fp fp_from_byte(int8_t v);   ///< a signed byte
/// Converts an unsigned byte to a number.
as_fp fp_from_uint8(uint8_t v);

/// Arithmetic. `a` is the left operand (the ROM's ARG, never has a rounding
/// byte), `f` the right one and the result.
void fp_add(as_fp *f, const as_fp *a);   ///< f = a + f
void fp_sub(as_fp *f, const as_fp *a);   ///< f = a - f
void fp_mul(as_fp *f, const as_fp *a);   ///< f = a * f
void fp_div(as_fp *f, const as_fp *a);   ///< f = a / f
void fp_pow(as_fp *f, const as_fp *a);   ///< f = a ^ f
/// Negates `*f` in place.
void fp_neg(as_fp *f);

/// Compares f with m: 0 equal, 1 f greater, -1 f less.
int fp_compare(const as_fp *f, const uint8_t *m);
int fp_sign(const as_fp *f);  ///< -1, 0, 1

/// The ROM's MUL10 and DIV10 (round first, then scale by ten).
void fp_mul10(as_fp *f);
/// Divides `*f` by ten in place.
void fp_div10(as_fp *f);

/// QINT: f's integer part (floor) as 32-bit two's complement, in f->mant.
/// Only for exponents below 160.
void fp_qint(as_fp *f);
void fp_int(as_fp *f);  ///< INT(): floor
/// INT's low byte of the integer, as the ROM leaves it for EXP and ^.
extern uint8_t fp_int_low;

/// The ROM's AYINT: f to -32768..32767 (floor) or ILLEGAL QUANTITY.
int fp_to_int16(as_fp *f);
/// GETADR: 0..65535 (negatives wrap, as -16384 = 49152) or ILLEGAL QUANTITY.
uint16_t fp_to_addr(as_fp *f);

/// Square root of `*f`, in place (SQR).
void fp_sqr(as_fp *f);
/// Natural logarithm of `*f`, in place (LOG).
void fp_log(as_fp *f);
/// e to the power of `*f`, in place (EXP).
void fp_exp(as_fp *f);
/// Sine of `*f` (radians), in place (SIN).
void fp_sin(as_fp *f);
/// Cosine of `*f` (radians), in place (COS).
void fp_cos(as_fp *f);
/// Tangent of `*f` (radians), in place (TAN).
void fp_tan(as_fp *f);
/// Arctangent of `*f`, in radians, in place (ATN).
void fp_atn(as_fp *f);
/// RND: seed is the 5-byte RNDSEED in zero page ($C9).
void fp_rnd(as_fp *f, uint8_t *seed);

/// Text to number, as the ROM's FIN reads a program: spaces are skipped, and
/// '+' and '-' may be the tokens $C8 and $C9 after the E. `next` returns the
/// next character (after any spaces) and advances; `peek` returns it without
/// advancing. Stops at the first character that isn't part of the number.
typedef struct {
    int (*peek)(void *ctx);  ///< the next character, without taking it (or -1 at the end)
    void (*next)(void *ctx);  ///< takes the character
    void *ctx;  ///< passed to peek and next
} as_fin_src;
/// Reads a number in Applesoft's syntax from `src`, as VAL and INPUT do.
as_fp fp_parse(const as_fin_src *src);

/// Number to text (STR$ and PRINT), at most 16 characters plus the NUL.
/// Rounds f in place, as the ROM does.
void fp_format(as_fp *f, char *out);
