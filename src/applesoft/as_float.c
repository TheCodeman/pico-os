/**
 * @file as_float.c
 * @brief Implementation of as_float.h: applesoft's floating point: 40-bit numbers (8-bit exponent, 32-bit mantissa) in a "floating accumulator" that carries an extra rounding byte.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */

#include "as_float.h"

#include <string.h>

void (*as_fp_error)(int code);
uint8_t fp_int_low;

/// The 40 bits of mantissa and rounding byte.
#define MASK40 0xffffffffffull
/// Their top bit.
#define BIT39  0x8000000000ull

/// Constants, in the packed form the ROM keeps them.
static const as_packed C_ONE = {0x81, 0x00, 0x00, 0x00, 0x00};
/// 0.5
static const as_packed C_HALF = {0x80, 0x00, 0x00, 0x00, 0x00};
/// -0.5
static const as_packed C_NEG_HALF = {0x80, 0x80, 0x00, 0x00, 0x00};
/// 0.25
static const as_packed C_QUARTER = {0x7f, 0x00, 0x00, 0x00, 0x00};
/// 10
static const as_packed C_TEN = {0x84, 0x20, 0x00, 0x00, 0x00};
/// SQR(0.5)
static const as_packed C_SQR_HALF = {0x80, 0x35, 0x04, 0xf3, 0x34};
/// SQR(2)
static const as_packed C_SQR_TWO = {0x81, 0x35, 0x04, 0xf3, 0x34};
/// LOG(2)
static const as_packed C_LN2 = {0x80, 0x31, 0x72, 0x17, 0xf8};
/// 1 / LOG(2)
static const as_packed C_LOG2E = {0x81, 0x38, 0xaa, 0x3b, 0x29};
/// PI / 2
static const as_packed C_HALF_PI = {0x81, 0x49, 0x0f, 0xda, 0xa2};
/// 2 * PI
static const as_packed C_TWO_PI = {0x83, 0x49, 0x0f, 0xda, 0xa2};
/// -32768 with a stray low bit, so -32768 itself is out of range, as on the Apple.
static const as_packed C_NEG32768 = {0x90, 0x80, 0x00, 0x00, 0x20};
static const as_packed C_FOUT_LO = {0x9b, 0x3e, 0xbc, 0x1f, 0xfd};  ///< 99999999.90625
static const as_packed C_FOUT_HI = {0x9e, 0x6e, 0x6b, 0x27, 0xfd};  ///< 999999999.25
/// 1E9
static const as_packed C_1E9 = {0x9e, 0x6e, 0x6b, 0x28, 0x00};
/// RND's multiplier.
static const as_packed C_RND_MUL = {0x98, 0x35, 0x44, 0x7a, 0x68};
/// RND's addend.
static const as_packed C_RND_ADD = {0x68, 0x28, 0xb1, 0x46, 0x20};

/// Polynomials: the number of terms less one, then the coefficients, highest
/// power first.
static const uint8_t P_LOG[] = {
    0x03, 0x7f, 0x5e, 0x56, 0xcb, 0x79, 0x80, 0x13, 0x9b, 0x0b, 0x64,
    0x80, 0x76, 0x38, 0x93, 0x16, 0x82, 0x38, 0xaa, 0x3b, 0x20,
};
/// EXP's polynomial.
static const uint8_t P_EXP[] = {
    0x07, 0x71, 0x34, 0x58, 0x3e, 0x56, 0x74, 0x16, 0x7e, 0xb3, 0x1b, 0x77, 0x2f, 0xee,
    0xe3, 0x85, 0x7a, 0x1d, 0x84, 0x1c, 0x2a, 0x7c, 0x63, 0x59, 0x58, 0x0a, 0x7e, 0x75,
    0xfd, 0xe7, 0xc6, 0x80, 0x31, 0x72, 0x18, 0x10, 0x81, 0x00, 0x00, 0x00, 0x00,
};
/// SIN's polynomial.
static const uint8_t P_SIN[] = {
    0x05, 0x84, 0xe6, 0x1a, 0x2d, 0x1b, 0x86, 0x28, 0x07, 0xfb, 0xf8,
    0x87, 0x99, 0x68, 0x89, 0x01, 0x87, 0x23, 0x35, 0xdf, 0xe1, 0x86,
    0xa5, 0x5d, 0xe7, 0x28, 0x83, 0x49, 0x0f, 0xda, 0xa2,
};
/// ATN's polynomial.
static const uint8_t P_ATN[] = {
    0x0b, 0x76, 0xb3, 0x83, 0xbd, 0xd3, 0x79, 0x1e, 0xf4, 0xa6, 0xf5, 0x7b, 0x83,
    0xfc, 0xb0, 0x10, 0x7c, 0x0c, 0x1f, 0x67, 0xca, 0x7c, 0xde, 0x53, 0xcb, 0xc1,
    0x7d, 0x14, 0x64, 0x70, 0x4c, 0x7d, 0xb7, 0xea, 0x51, 0x7a, 0x7d, 0x63, 0x30,
    0x88, 0x7e, 0x7e, 0x92, 0x44, 0x99, 0x3a, 0x7e, 0x4c, 0xcc, 0x91, 0xc7, 0x7f,
    0xaa, 0xaa, 0xaa, 0x13, 0x81, 0x00, 0x00, 0x00, 0x00,
};

/// Raises an Applesoft error.
/// @param code the error
static void fail(int code) {
    as_fp_error(code);
}

/// The mantissa and rounding byte as one 40-bit number.
/// @param f the number
/// @return the 40 bits
static inline uint64_t wide(const as_fp *f) {
    return (uint64_t)f->mant << 8 | f->ext;
}

/// Sets the mantissa and rounding byte from 40 bits.
/// @param f the number
/// @param v the 40 bits
static inline void set_wide(as_fp *f, uint64_t v) {
    f->mant = (uint32_t)(v >> 8);
    f->ext = (uint8_t)v;
}

/// Sets a number to zero.
/// @param f the number
static inline void set_zero(as_fp *f) {
    f->exp = 0;
    f->neg = false;
}

as_fp fp_unpack(const uint8_t *m) {
    as_fp f;
    f.exp = m[0];
    f.neg = m[1] & 0x80;
    f.mant = (uint32_t)(m[1] | 0x80) << 24 | (uint32_t)m[2] << 16 | (uint32_t)m[3] << 8 | m[4];
    f.ext = 0;
    return f;
}

void fp_round(as_fp *f) {
    if (!f->exp)
        return;
    bool up = f->ext & 0x80;
    f->ext <<= 1;
    if (!up)
        return;
    if (++f->mant == 0) {
        if (++f->exp == 0)
            fail(AS_ERR_OVERFLOW);
        f->mant = 0x80000000u;
        f->ext >>= 1;
    }
}

void fp_pack(as_fp *f, uint8_t *m) {
    fp_round(f);
    m[0] = f->exp;
    m[1] = (uint8_t)((f->mant >> 24) & (f->neg ? 0xff : 0x7f));
    m[2] = (uint8_t)(f->mant >> 16);
    m[3] = (uint8_t)(f->mant >> 8);
    m[4] = (uint8_t)f->mant;
    f->ext = 0;
}

/// The ROM's FAC to ARG: rounded, without the rounding byte.
/// @param f the FAC
/// @return the ARG
static as_fp to_arg(as_fp *f) {
    fp_round(f);
    f->ext = 0;
    as_fp a = *f;
    return a;
}

/// Shifts the rounding byte and mantissa left until the top bit is set,
/// taking it off the exponent; zero (or underflow) gives zero.
/// @param f the number
static void normalize(as_fp *f) {
    uint64_t v = wide(f);
    int count = 0;
    while (!(v >> 32)) {  // top mantissa byte is zero
        v = (v << 8) & MASK40;
        count += 8;
        if (count == 32) {
            set_wide(f, v);
            set_zero(f);
            return;
        }
    }
    while (!(v & BIT39)) {
        v <<= 1;
        count++;
    }
    v &= MASK40;
    set_wide(f, v);
    if (count >= f->exp) {
        set_zero(f);
        return;
    }
    f->exp -= count;
}

/// Negates the 40-bit mantissa (two's complement), flipping the sign.
/// @param f the number
static void negate_wide(as_fp *f) {
    f->neg = !f->neg;
    set_wide(f, (~wide(f) + 1) & MASK40);
}

/// The ROM's FADDT: f = a + f, where `differ` says the signs differ (ROM's
/// sign comparison byte, worked out when ARG was loaded).
/// @param f the FAC, and the sum
/// @param a the ARG
/// @param differ the signs differ
static void add_core(as_fp *f, const as_fp *a, bool differ) {
    if (!f->exp) {
        f->exp = a->exp;
        f->mant = a->mant;
        f->neg = a->neg;
        f->ext = 0;
        return;
    }
    if (!a->exp)
        return;
    int d = a->exp - f->exp;
    uint64_t big, small;
    unsigned carry_in = 0;
    if (d < 0) {
        big = wide(f);
        small = -d >= 40 ? 0 : ((uint64_t)a->mant << 8) >> -d;
    } else if (d > 0) {
        f->exp = a->exp;
        f->neg = a->neg;
        small = d >= 40 ? 0 : wide(f) >> d;
        big = (uint64_t)a->mant << 8;
    } else {
        big = wide(f);
        small = (uint64_t)a->mant << 8;
        carry_in = 1;  // the ROM adds with the carry still set here
    }
    if (!differ) {
        uint64_t sum = big + small + carry_in;
        if (sum >> 40) {
            if (++f->exp == 0)
                fail(AS_ERR_OVERFLOW);
            sum = (sum >> 1) | BIT39;
        }
        set_wide(f, sum & MASK40);
        return;
    }
    uint64_t diff = (big - small) & MASK40;
    set_wide(f, diff);
    if (big < small)
        negate_wide(f);
    normalize(f);
}

void fp_add(as_fp *f, const as_fp *a) {
    add_core(f, a, f->neg != a->neg);
}

void fp_sub(as_fp *f, const as_fp *a) {
    f->neg = !f->neg;
    add_core(f, a, f->neg != a->neg);
}

void fp_neg(as_fp *f) {
    if (f->exp)
        f->neg = !f->neg;
}

/// ADD.EXPONENTS for multiply and divide: e1 + e2 - 128 into f->exp. Returns
/// false when the result is zero (underflow); *first_carry is the carry the
/// ROM is left with, which the multiply loop goes on to use.
/// @param f the result
/// @param aexp the ARG's exponent
/// @param fexp the FAC's exponent
/// @param neg the result's sign
/// @param first_carry set to the first carry
/// @return false if the result is zero (or underflowed)
static bool add_exponents(as_fp *f, uint8_t aexp, uint8_t fexp, bool neg, int *first_carry) {
    if (!aexp) {
        set_zero(f);
        return false;
    }
    int sum = aexp + fexp;
    if (sum < 256) {
        if (sum < 128) {
            set_zero(f);
            return false;
        }
        *first_carry = 1;
    } else {
        if (sum - 256 >= 128)
            fail(AS_ERR_OVERFLOW);
        *first_carry = 0;
    }
    f->exp = (uint8_t)(sum - 128);
    // An exponent of exactly zero goes on (with the sign cleared): a multiply
    // then comes out zero, but a divide adds one to it.
    f->neg = f->exp ? neg : false;
    return true;
}

/// The ROM's FMULTT: f = a * f.
/// @param f the FAC, and the product
/// @param a the ARG
/// @param neg the product's sign
static void mul_core(as_fp *f, const as_fp *a, bool neg) {
    if (!f->exp)
        return;
    int carry;
    uint8_t bytes[5] = {f->ext, (uint8_t)f->mant, (uint8_t)(f->mant >> 8),
                        (uint8_t)(f->mant >> 16), (uint8_t)(f->mant >> 24)};
    if (!add_exponents(f, a->exp, f->exp, neg, &carry))
        return;
    uint64_t r = 0;  // 32-bit product and its rounding byte below
    for (int i = 0; i < 5; ++i) {
        uint8_t b = bytes[i];
        if (b == 0 && i < 4) {
            r >>= 8;
            if (!carry) {
                // The ROM shifts the product one bit further, but not the
                // rounding byte below it.
                uint32_t hi = (uint32_t)(r >> 8) >> 1;
                r = (uint64_t)hi << 8 | (r & 0xff);
            }
            carry = 0;
            continue;
        }
        for (int bit = 0; bit < 8; ++bit) {
            uint64_t in = 0;
            if (b >> bit & 1) {
                uint64_t s = (r >> 8) + a->mant;
                in = s >> 32;
                r = (s & 0xffffffffu) << 8 | (r & 0xff);
            }
            r = (r >> 1) | (in << 39);
        }
        carry = 1;
    }
    set_wide(f, r);
    normalize(f);
}

void fp_mul(as_fp *f, const as_fp *a) {
    mul_core(f, a, f->neg != a->neg);
}

/// The ROM's FDIVT: f = a / f.
/// @param f the FAC (the divisor), and the quotient
/// @param a the ARG (the dividend)
/// @param neg the quotient's sign
static void div_core(as_fp *f, const as_fp *a, bool neg) {
    if (!f->exp)
        fail(AS_ERR_DIVZERO);
    fp_round(f);
    int carry;
    uint32_t d = f->mant;
    if (!add_exponents(f, a->exp, (uint8_t)(256 - f->exp), neg, &carry))
        return;
    if (++f->exp == 0)
        fail(AS_ERR_OVERFLOW);
    uint64_t rem = a->mant, q = 0;
    for (int i = 0; i < 34; ++i) {
        unsigned bit = rem >= d;
        if (bit)
            rem -= d;
        rem <<= 1;
        q = q << 1 | bit;
    }
    f->mant = (uint32_t)(q >> 2);
    f->ext = (uint8_t)((q & 3) << 6);
    normalize(f);
}

void fp_div(as_fp *f, const as_fp *a) {
    div_core(f, a, f->neg != a->neg);
}

int fp_sign(const as_fp *f) {
    if (!f->exp)
        return 0;
    return f->neg ? -1 : 1;
}

int fp_compare(const as_fp *f, const uint8_t *m) {
    if (!m[0])
        return fp_sign(f);
    bool mneg = m[1] & 0x80;
    if (mneg != f->neg)
        return f->neg ? -1 : 1;
    bool m_bigger;  // the ROM's carry: |m| >= |f|
    if (m[0] != f->exp) {
        m_bigger = m[0] > f->exp;
    } else {
        uint8_t fb[4] = {(uint8_t)(f->mant >> 24), (uint8_t)(f->mant >> 16),
                         (uint8_t)(f->mant >> 8), (uint8_t)f->mant};
        uint8_t mb[4] = {(uint8_t)(m[1] | 0x80), m[2], m[3], m[4]};
        int i = 0;
        while (i < 3 && mb[i] == fb[i])
            ++i;
        if (i < 3) {
            m_bigger = mb[i] > fb[i];
        } else {
            int r = f->ext >= 0x80;
            int diff = mb[3] - fb[3] - r;
            if ((diff & 0xff) == 0)
                return 0;
            m_bigger = diff >= 0;
        }
    }
    // |f| < |m|: f is less if positive.
    bool f_less = m_bigger != f->neg;
    return f_less ? -1 : 1;
}

/// Compares f with a packed constant.
/// @param f the number
/// @param m the constant
/// @return -1, 0 or 1
static int compare_fp(const as_fp *f, const as_packed m) {
    return fp_compare(f, m);
}

as_fp fp_from_byte(int8_t v) {
    as_fp f = {0x88, (uint32_t)(uint8_t)v << 24, 0, false};
    if (v < 0)
        negate_wide(&f);
    normalize(&f);
    return f;
}

as_fp fp_from_int16(int v) {
    as_fp f = {0x90, (uint32_t)(uint16_t)v << 16, 0, false};
    if (v & 0x8000)
        negate_wide(&f);
    normalize(&f);
    return f;
}

as_fp fp_from_uint8(uint8_t v) {
    return fp_from_int16(v);
}

void fp_qint(as_fp *f) {
    if (!f->exp) {
        f->mant = 0;
        return;
    }
    int s = 160 - f->exp;
    int64_t v = (int64_t)wide(f);
    if (f->neg)
        v = -v;
    v = s >= 40 ? (v < 0 ? -1 : 0) : v >> s;  // arithmetic: floor
    // The ROM leaves the rounding byte holding the last byte it shifted out,
    // which nothing after QINT depends on.
    f->mant = (uint32_t)(v >> 8);
    f->ext = (uint8_t)v;
}

void fp_int(as_fp *f) {
    if (f->exp >= 160)
        return;
    bool neg = f->neg;
    fp_qint(f);
    f->ext = 0;
    f->neg = false;
    f->exp = 160;
    fp_int_low = (uint8_t)f->mant;
    if (neg)
        negate_wide(f);
    normalize(f);
}

int fp_to_int16(as_fp *f) {
    if (f->exp >= 0x90 && compare_fp(f, C_NEG32768) != 0)
        fail(AS_ERR_ILLQTY);
    fp_qint(f);
    return (int16_t)(f->mant & 0xffff);
}

uint16_t fp_to_addr(as_fp *f) {
    if (f->exp >= 0x91)
        fail(AS_ERR_ILLQTY);
    fp_qint(f);
    return (uint16_t)f->mant;
}

void fp_mul10(as_fp *f) {
    as_fp a = to_arg(f);
    if (!a.exp)
        return;
    if (a.exp + 2 > 255)
        fail(AS_ERR_OVERFLOW);
    // x*4 + x, the ARG copy's exponent raised by two, then doubled.
    a.exp += 2;
    add_core(f, &a, false);
    if (++f->exp == 0)
        fail(AS_ERR_OVERFLOW);
}

void fp_div10(as_fp *f) {
    as_fp a = to_arg(f);
    *f = fp_unpack(C_TEN);
    div_core(f, &a, false);
}

// ---- Polynomials and the functions built on them -------------------------------

/// A packed number unpacked.
/// @param m the 5 bytes
/// @return the number
static as_fp from_packed(const uint8_t *m) {
    return fp_unpack(m);
}

/// f = m * f.
/// @param f the number
/// @param m a packed number
static void mul_mem(as_fp *f, const uint8_t *m) {
    as_fp a = from_packed(m);
    fp_mul(f, &a);
}

/// f = m + f.
/// @param f the number
/// @param m a packed number
static void add_mem(as_fp *f, const uint8_t *m) {
    as_fp a = from_packed(m);
    fp_add(f, &a);
}

/// m - f
/// @param f the number
/// @param m a packed number
static void sub_from_mem(as_fp *f, const uint8_t *m) {
    as_fp a = from_packed(m);
    fp_sub(f, &a);
}

/// m / f
/// @param f the number
/// @param m a packed number
static void div_into_mem(as_fp *f, const uint8_t *m) {
    as_fp a = from_packed(m);
    fp_div(f, &a);
}

/// POLY1: c0*y^n + ... + cn by Horner's rule, y being f (kept rounded).
/// @param f the number
/// @param table the polynomial
static void poly(as_fp *f, const uint8_t *table) {
    as_packed y;
    fp_pack(f, y);
    int n = table[0];
    const uint8_t *c = table + 1;
    mul_mem(f, c);
    for (;;) {
        c += 5;
        add_mem(f, c);
        if (--n == 0)
            break;
        mul_mem(f, y);
    }
}

/// POLY_ODD: x * P(x^2).
/// @param f the number
/// @param table the polynomial
static void poly_odd(as_fp *f, const uint8_t *table) {
    as_packed x;
    fp_pack(f, x);
    mul_mem(f, x);
    poly(f, table);
    mul_mem(f, x);
}

void fp_log(as_fp *f) {
    if (fp_sign(f) <= 0)
        fail(AS_ERR_ILLQTY);
    int8_t e = (int8_t)(f->exp - 0x80);
    f->exp = 0x80;
    add_mem(f, C_SQR_HALF);
    div_into_mem(f, C_SQR_TWO);
    sub_from_mem(f, C_ONE);
    poly_odd(f, P_LOG);
    add_mem(f, C_NEG_HALF);
    // + the exponent, as the ROM adds a digit in FIN
    as_fp a = to_arg(f);
    *f = fp_from_byte(e);
    add_core(f, &a, a.neg != f->neg);
    mul_mem(f, C_LN2);
}

void fp_exp(as_fp *f) {
    mul_mem(f, C_LOG2E);
    // The ROM rounds up when the rounding byte is 0xB0 or more, and keeps the
    // byte plus 0x50 for later.
    uint8_t saved_ext = (uint8_t)(f->ext + 0x50);
    if (f->ext >= 0xb0) {
        if (++f->mant == 0) {
            if (++f->exp == 0)
                fail(AS_ERR_OVERFLOW);
            f->mant = 0x80000000u;
        }
    }
    f->ext = 0;
    as_fp y = *f;  // copied without rounding
    if (f->exp >= 0x88) {
    too_big:
        if (!f->neg)
            fail(AS_ERR_OVERFLOW);
        set_zero(f);
        return;
    }
    fp_int(f);
    uint8_t k = (uint8_t)(fp_int_low + 0x81);
    if (k == 0)
        goto too_big;
    k -= 1;  // integer + 0x80: the result's exponent bias
    // Swap: f is y again (with the saved rounding byte), the integer in ARG.
    as_fp ipart = *f;
    *f = y;
    f->ext = saved_ext;
    fp_sub(f, &ipart);  // integer - y
    fp_neg(f);          // the fraction, 0 <= y - int < 1
    poly(f, P_EXP);
    // Add k - 0x80 to the exponent (the ROM's ADD.EXPONENTS with sign 0).
    if (!k) {
        set_zero(f);
        return;
    }
    int carry;
    add_exponents(f, k, f->exp, false, &carry);
}

void fp_pow(as_fp *f, const as_fp *a) {
    if (!f->exp) {
        fp_exp(f);  // anything ^ 0 = 1
        return;
    }
    if (!a->exp) {
        set_zero(f);
        return;
    }
    as_packed y;
    fp_pack(f, y);
    uint8_t parity = 0;
    bool log_neg = false;  // a negative base needs an integer power
    if (a->neg) {
        fp_int(f);
        int c = fp_compare(f, y);
        if (c == 0)
            parity = fp_int_low;
        else
            log_neg = c < 0;
    }
    f->exp = a->exp;
    f->mant = a->mant;
    f->ext = 0;
    f->neg = log_neg;
    fp_log(f);
    mul_mem(f, y);
    fp_exp(f);
    if (parity & 1)
        fp_neg(f);
}

void fp_sqr(as_fp *f) {
    as_fp a = to_arg(f);
    *f = fp_unpack(C_HALF);
    fp_pow(f, &a);
}

/// Set by SIN when the reduced angle needs the cosine's sign flipped (TAN).
static bool tan_flip;
static as_packed sin_reduced;  ///< what SIN left in TEMP1, which TAN reuses

/// SIN's end, shared with COS and TAN: 0.25 - f, then the polynomial.
/// @param f the reduced argument
/// @param push_neg negate first (the ROM's sign on the stack)
static void sin_tail(as_fp *f, bool push_neg) {
    fp_neg(f);
    add_mem(f, C_QUARTER);
    if (push_neg)
        fp_neg(f);
    // poly_odd stores its argument in TEMP1 first; TAN reads it back.
    as_fp t = *f;
    fp_pack(&t, sin_reduced);
    poly_odd(f, P_SIN);
}

void fp_sin(as_fp *f) {
    as_fp a = to_arg(f);
    *f = fp_unpack(C_TWO_PI);
    div_core(f, &a, a.neg);  // x / 2pi
    a = to_arg(f);
    fp_int(f);
    fp_sub(f, &a);  // frac = x/2pi - int
    sub_from_mem(f, C_QUARTER);  // 0.25 - frac
    bool neg = f->neg;
    if (!neg) {
        sin_tail(f, false);
        return;
    }
    add_mem(f, C_HALF);
    if (f->neg) {
        add_mem(f, C_QUARTER);
        fp_neg(f);
        as_fp t = *f;
        fp_pack(&t, sin_reduced);
        poly_odd(f, P_SIN);
        return;
    }
    tan_flip = !tan_flip;
    sin_tail(f, true);
}

void fp_cos(as_fp *f) {
    add_mem(f, C_HALF_PI);
    fp_sin(f);
}

void fp_tan(as_fp *f) {
    as_packed x;
    fp_pack(f, x);
    (void)x;
    tan_flip = false;
    fp_sin(f);
    as_packed s;
    fp_pack(f, s);
    *f = fp_unpack(sin_reduced);
    f->neg = false;
    sin_tail(f, tan_flip);
    div_into_mem(f, s);
}

void fp_atn(as_fp *f) {
    bool neg = f->neg;
    if (neg)
        fp_neg(f);
    uint8_t e = f->exp;
    if (e >= 0x81)
        div_into_mem(f, C_ONE);
    poly_odd(f, P_ATN);
    if (e >= 0x81)
        sub_from_mem(f, C_HALF_PI);
    if (neg)
        fp_neg(f);
}

void fp_rnd(as_fp *f, uint8_t *seed) {
    int s = fp_sign(f);
    if (s >= 0) {
        *f = fp_unpack(seed);
        if (s == 0)
            return;
        mul_mem(f, C_RND_MUL);
        add_mem(f, C_RND_ADD);
    }
    uint8_t m1 = (uint8_t)(f->mant >> 24), m4 = (uint8_t)f->mant;
    f->mant = (f->mant & 0x00ffff00u) | (uint32_t)m4 << 24 | m1;
    f->neg = false;
    f->ext = f->exp;
    f->exp = 0x80;
    normalize(f);
    fp_pack(f, seed);
}

// ---- Text ----------------------------------------------------------------------

/// Adds a digit to the number being read: f = f * 10 + digit, as the ROM does it.
/// @param f the number
/// @param digit the digit
static void add_digit(as_fp *f, int digit) {
    as_fp a = to_arg(f);
    *f = fp_from_byte((int8_t)digit);
    add_core(f, &a, a.neg != f->neg);
}

/// Whether c is a decimal digit.
/// @param c the character
/// @return it is
static bool is_digit(int c) {
    return c >= '0' && c <= '9';
}

as_fp fp_parse(const as_fin_src *src) {
    as_fp f = {0, 0, 0, false};
    uint8_t decimals = 0, exp10 = 0;
    bool point = false, neg = false, exp_neg = false;
    int c = src->peek(src->ctx);
    if (c == '-' || c == '+') {
        neg = c == '-';
        src->next(src->ctx);
        c = src->peek(src->ctx);
    }
    for (;;) {
        if (is_digit(c)) {
            if (point)
                decimals++;
            fp_mul10(&f);
            add_digit(&f, c - '0');
        } else if (c == '.') {
            if (point)
                break;
            point = true;
        } else if (c == 'E') {
            src->next(src->ctx);
            c = src->peek(src->ctx);
            if (c == 0xc9 || c == '-') {
                exp_neg = true;
                src->next(src->ctx);
                c = src->peek(src->ctx);
            } else if (c == 0xc8 || c == '+') {
                src->next(src->ctx);
                c = src->peek(src->ctx);
            }
            while (is_digit(c)) {
                if (exp10 >= 10) {
                    if (!exp_neg)
                        fail(AS_ERR_OVERFLOW);
                    exp10 = 100;
                } else {
                    exp10 = (uint8_t)(exp10 * 10 + c - '0');
                }
                src->next(src->ctx);
                c = src->peek(src->ctx);
            }
            break;
        } else {
            break;
        }
        src->next(src->ctx);
        c = src->peek(src->ctx);
    }
    uint8_t e = exp_neg ? (uint8_t)-exp10 : exp10;
    e = (uint8_t)(e - decimals);
    if (e) {
        if (!(e & 0x80)) {
            do
                fp_mul10(&f);
            while (--e);
        } else {
            do
                fp_div10(&f);
            while (++e);
        }
    }
    if (neg)
        fp_neg(&f);
    return f;
}

void fp_format(as_fp *f, char *out) {
    char *p = out;
    if (f->neg)
        *p++ = '-';
    f->neg = false;
    if (!f->exp) {
        *p++ = '0';
        *p = 0;
        return;
    }
    int8_t dexp = 0;  // the ROM's $99
    if (f->exp <= 0x80) {
        mul_mem(f, C_1E9);
        dexp = -9;
    }
    // Scale into 99999999.9 < f <= 999999999.25, then round and take the
    // integer: nine digits. (The loop's odd exits, when the count reaches
    // zero, are the ROM's.)
    int c;
too_big:
    c = compare_fp(f, C_FOUT_HI);
    if (c == 0)
        goto exact;
    if (c > 0)
        goto divide;
too_small:
    if (compare_fp(f, C_FOUT_LO) > 0)
        goto round;
    fp_mul10(f);
    if (--dexp != 0)
        goto too_small;
divide:
    fp_div10(f);
    if (++dexp != 0)
        goto too_big;
round:
    add_mem(f, C_HALF);
exact:
    fp_qint(f);
    uint32_t n = f->mant;

    int point;      // digits before the decimal point (the ROM's X)
    int sci = 0;    // exponent shown after E, 0 for none
    int t = dexp + 10;
    if (t < 0 || t >= 11) {
        sci = t - 2;
        point = 1;
    } else {
        point = t - 1;
    }
    if (point <= 0) {
        *p++ = '.';
        if (point < 0)
            *p++ = '0';
    }
    char digits[10];
    for (int i = 8; i >= 0; --i) {
        digits[i] = (char)('0' + n % 10);
        n /= 10;
    }
    for (int i = 0; i < 9; ++i) {
        *p++ = digits[i];
        if (i + 1 == point)
            *p++ = '.';
    }
    // Trailing zeros go, and the point if nothing follows it.
    while (p[-1] == '0')
        --p;
    if (p[-1] == '.')
        --p;
    if (sci) {
        *p++ = 'E';
        *p++ = sci < 0 ? '-' : '+';
        int a = sci < 0 ? -sci : sci;
        *p++ = (char)('0' + a / 10);
        *p++ = (char)('0' + a % 10);
    }
    *p = 0;
}
