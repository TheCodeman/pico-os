/**
 * @file as_mach.c
 * @brief The rest of the machine: the I/O page (keyboard, speaker, display switches, game controller), PEEK and POKE, and machine code run by CALL, USR and & on the 65C02.
 *
 * Calls into the monitor and Applesoft ROMs (which we don't have) are caught
 * at their entry points and done natively.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */

#include <string.h>

#include "as_cpu.h"
#include "as_internal.h"
#include "as_port.h"

static uint64_t cpu_base_us;   ///< asp_micros() when the 6502 started running
static uint32_t cpu_start_cycles;
static bool cpu_running;
static as_cpu cpu;
static uint32_t paddle_trigger;  ///< cycle count at the last $C070

uint64_t as_now_us(void) {
    if (cpu_running)
        return cpu_base_us + (uint64_t)(cpu.cycles - cpu_start_cycles) * 1000 / 1023;
    return asp_micros();
}

void as_click(void) {
    asp_speaker_click(as_now_us());
}

// ---- The I/O page ------------------------------------------------------------------

uint8_t as_softswitch(uint16_t a, bool write, uint8_t v) {
    uint8_t r = 0;
    bool mode_change = false;
    switch (a & 0xff) {
    case 0x00:
        if (write) {
            vid.store80 = false;
            mode_change = true;
        } else {
            r = as_kbd();
        }
        break;
    case 0x01: if (write) vid.store80 = true, mode_change = true; else r = as_kbd(); break;
    case 0x0c: if (write) vid.col80 = false, mode_change = true; break;
    case 0x0d: if (write) vid.col80 = true, mode_change = true; break;
    case 0x0e: if (write) vid.altchar = false, mode_change = true; break;
    case 0x0f: if (write) vid.altchar = true, mode_change = true; break;
    case 0x10:
        r = as_kbd() & 0x7f;
        as_kbd_strobe();
        break;
    case 0x18: r = vid.store80 ? 0x80 : 0; break;
    case 0x19: r = (as_now_us() % 16667) < 4500 ? 0 : 0x80; break;  // VBL
    case 0x1a: r = vid.text ? 0x80 : 0; break;
    case 0x1b: r = vid.mixed ? 0x80 : 0; break;
    case 0x1c: r = vid.page2 ? 0x80 : 0; break;
    case 0x1d: r = vid.hires ? 0x80 : 0; break;
    case 0x1e: r = vid.altchar ? 0x80 : 0; break;
    case 0x1f: r = vid.col80 ? 0x80 : 0; break;
    case 0x30: as_click(); break;
    case 0x50: vid.text = false; mode_change = true; break;
    case 0x51: vid.text = true; mode_change = true; break;
    case 0x52: vid.mixed = false; mode_change = true; break;
    case 0x53: vid.mixed = true; mode_change = true; break;
    case 0x54: vid.page2 = false; mode_change = true; break;
    case 0x55: vid.page2 = true; mode_change = true; break;
    case 0x56: vid.hires = false; mode_change = true; break;
    case 0x57: vid.hires = true; mode_change = true; break;
    case 0x61: case 0x62: case 0x63:
        r = asp_button((a & 0xff) - 0x61) ? 0x80 : 0;
        break;
    case 0x64: case 0x65: case 0x66: case 0x67: {
        // The paddle timer runs out after about 11 cycles per unit.
        uint32_t since = cpu.cycles - paddle_trigger;
        r = since < (uint32_t)asp_paddle((a & 0xff) - 0x64) * 11 ? 0x80 : 0;
        break;
    }
    case 0x70: paddle_trigger = cpu.cycles; break;
    default: break;
    }
    if (mode_change)
        video_mark_all();
    (void)v;
    return r;
}

uint8_t as_peek(uint16_t a) {
    if (a < 0x100)
        as_sync_out();
    if (a < 0xc000 || a >= 0xd000)
        return M[a];
    if (a < 0xc100)
        return as_softswitch(a, false, 0);
    return 0xff;
}

void as_poke(uint16_t a, uint8_t v) {
    if (a < 0xc000) {
        if (a < 0x100)
            as_sync_out();
        M[a] = v;
        video_mark(a);
        if (a < 0x100)
            as_sync_in();
        return;
    }
    if (a < 0xc100)
        as_softswitch(a, true, v);
}

// ---- The 65C02 ---------------------------------------------------------------------

static uint8_t cpu_read(void *ctx, uint16_t a) {
    (void)ctx;
    if (a < 0xc000 || a >= 0xd000)
        return M[a];
    if (a < 0xc100)
        return as_softswitch(a, false, 0);
    return 0xff;
}

static void cpu_write(void *ctx, uint16_t a, uint8_t v) {
    (void)ctx;
    if (a < 0xc000) {
        M[a] = v;
        video_mark(a);
    } else if (a < 0xc100) {
        as_softswitch(a, true, v);
    }
}

#define RETURN_TRAP 0xfff0  ///< where CALL's return address points

static uint8_t pull(void) {
    cpu.s++;
    return M[0x100 | cpu.s];
}

static void rts(void) {
    uint16_t lo = pull();
    cpu.pc = (uint16_t)((lo | pull() << 8) + 1);
}

/// Applesoft routines need the interpreter's state, which lives in zero page
/// while machine code runs.
static void basic_in(void) {
    as_sync_in();
}

static void basic_out(void) {
    as_sync_out();
    cpu.a = M[Z_FAC];
}

/// The FAC as machine code left it in zero page, and back.
static void fac_in(void) {
    fac.exp = M[Z_FAC];
    fac.mant = (uint32_t)M[Z_FAC + 1] << 24 | (uint32_t)M[Z_FAC + 2] << 16 |
               (uint32_t)M[Z_FAC + 3] << 8 | M[Z_FAC + 4];
    fac.neg = M[Z_FAC + 5] & 0x80;
    fac.ext = M[0xac];
    fac_string = false;
}

static void fac_out(void) {
    M[Z_FAC] = fac.exp;
    M[Z_FAC + 1] = (uint8_t)(fac.mant >> 24);
    M[Z_FAC + 2] = (uint8_t)(fac.mant >> 16);
    M[Z_FAC + 3] = (uint8_t)(fac.mant >> 8);
    M[Z_FAC + 4] = (uint8_t)fac.mant;
    M[Z_FAC + 5] = fac.neg ? 0xff : 0;
    M[0xac] = fac.ext;
    cpu.a = fac.exp;
}

static as_fp arg_in(void) {
    as_fp a;
    a.exp = M[Z_ARG];
    a.mant = (uint32_t)M[Z_ARG + 1] << 24 | (uint32_t)M[Z_ARG + 2] << 16 |
             (uint32_t)M[Z_ARG + 3] << 8 | M[Z_ARG + 4];
    a.neg = M[Z_ARG + 5] & 0x80;
    a.ext = 0;
    return a;
}

/// FAC = (memory at A,Y) op FAC
static void fp_mem_op(void (*op)(as_fp *, const as_fp *)) {
    fac_in();
    as_fp a = fp_unpack(&M[(uint16_t)(cpu.a | cpu.y << 8)]);
    op(&fac, &a);
    fac_out();
}

static void fp_arg_op(void (*op)(as_fp *, const as_fp *)) {
    fac_in();
    as_fp a = arg_in();
    op(&fac, &a);
    fac_out();
}

static void fp_fn(void (*fn)(as_fp *)) {
    fac_in();
    fn(&fac);
    fac_out();
}

static void fp_rnd_trap(as_fp *f) {
    fp_rnd(f, &M[Z_RNDSEED]);
}

static void fp_sgn_trap(as_fp *f) {
    *f = fp_from_byte((int8_t)fp_sign(f));
}

static void print_hex(uint8_t v) {
    static const char hex[] = "0123456789ABCDEF";
    as_cout((uint8_t)(hex[v >> 4] | 0x80));
    as_cout((uint8_t)(hex[v & 15] | 0x80));
}

/// The monitor's WAIT: the cycles it takes, counted rather than spent.
static void wait_cycles(uint8_t a) {
    unsigned n = a ? a : 256;
    cpu.cycles += (26 + 27 * n + 5 * n * n) / 2;
}

/// Runs the ROM routine at pc natively. Returns false if we don't have it.
static bool rom_trap(uint16_t pc) {
    switch (pc) {
    // Monitor: screen and keyboard
    case 0xfded: as_cout(cpu.a); break;
    case 0xfdf0: case 0xfdf6: as_cout(cpu.a); break;
    case 0xfd8e: as_cout(0x8d); cpu.a = 0x8d; break;
    case 0xfd8b: as_clreol(); as_cout(0x8d); cpu.a = 0x8d; break;
    case 0xfdda: print_hex(cpu.a); break;
    case 0xfde3: print_hex(cpu.a & 15); break;
    case 0xf941: print_hex(cpu.a); print_hex(cpu.x); break;
    case 0xf940: print_hex(cpu.y); print_hex(cpu.x); break;
    case 0xf948: for (int i = 0; i < 3; ++i) as_cout(0xa0); break;
    case 0xf94a: for (int i = 0; i < cpu.x; ++i) as_cout(0xa0); break;
    case 0xfd0c: case 0xfd1b: case 0xfd18: cpu.a = as_rdkey(); break;
    case 0xfd35: cpu.a = as_rdkey(); break;
    case 0xfd6a: cpu.x = (uint8_t)as_getln(); cpu.a = M[0x200 + cpu.x]; break;
    case 0xfd67: as_cout(0x8d); cpu.x = (uint8_t)as_getln(); break;
    case 0xff3a: as_cout(0x87); break;
    case 0xfbdd: case 0xfbe4: as_bell(); break;
    case 0xfc58: as_home(); break;
    case 0xfc42: as_clreop(); break;
    case 0xfc9c: as_clreol(); break;
    case 0xfc22: case 0xfc24: as_vtab(); break;
    case 0xfb5b: M[Z_CV] = cpu.a; as_vtab(); break;
    case 0xfbc1: {
        uint8_t r = cpu.a;
        sw16(Z_BASL, (uint16_t)(0x400 + (r & 7) * 128 + ((r >> 3) & 3) * 40));
        break;
    }
    case 0xfc62: as_cout(0x8d); break;
    case 0xfc66: as_cout(0x8a); break;
    case 0xfc10: as_cout(0x88); break;
    case 0xfca8: wait_cycles(cpu.a); cpu.a = 0; break;
    case 0xfb39: as_text_mode(); break;
    case 0xfb40: gfx_gr(); break;
    case 0xff4a: case 0xff4c: M[0x45] = cpu.a; M[0x46] = cpu.x; M[0x47] = cpu.y;
        M[0x48] = cpu.p; M[0x49] = cpu.s; break;
    case 0xff3f: cpu.a = M[0x45]; cpu.x = M[0x46]; cpu.y = M[0x47]; cpu.p = M[0x48]; break;
    case 0xff58: case 0xfadd: break;  // RTS
    case 0xfb1e: cpu.y = (uint8_t)asp_paddle(cpu.x & 3); break;
    // Monitor: lo-res
    case 0xf800: gfx_plot(cpu.y, cpu.a); break;
    case 0xf819: gfx_hlin(cpu.y, M[Z_H2], cpu.a); break;
    case 0xf828: gfx_vlin(cpu.a, M[Z_V2], cpu.y); break;
    case 0xf864: gfx_set_color(cpu.a); break;
    case 0xf85f: gfx_set_color((M[Z_COLOR] + 3) & 15); break;
    case 0xf871: cpu.a = (uint8_t)gfx_scrn(cpu.y, cpu.a); break;
    case 0xf832: case 0xf836: {
        int rows = pc == 0xf832 ? 48 : 40;
        uint8_t c = M[Z_COLOR];
        M[Z_COLOR] = 0;
        for (int y = 0; y < rows; ++y)
            for (int x = 0; x < 40; ++x)
                gfx_plot(x, y);
        M[Z_COLOR] = c;
        break;
    }
    // Applesoft: hi-res
    case 0xf3e2: gfx_hgr(1); break;
    case 0xf3d8: gfx_hgr(2); break;
    case 0xf3f2: M[0x1c] = 0; extern void gfx_hclear(void); gfx_hclear(); break;
    case 0xf3f6: gfx_hclear(); break;
    case 0xf411: gfx_hposn(cpu.x | cpu.y << 8, cpu.a); break;
    case 0xf457: gfx_hplot(cpu.x | cpu.y << 8, cpu.a); break;
    case 0xf53a: gfx_hplot_to(cpu.a | cpu.x << 8, cpu.y); break;
    // Applesoft: the interpreter
    case 0xf328: extern void as_restore_errstk(void); as_restore_errstk(); break;
    case 0xd412: as_error(cpu.x);
    case 0xdec9: as_error(ERR_SYNTAX);
    case 0xe199: as_error(ERR_ILLEGAL_QUANTITY);
    case 0xd410: as_error(ERR_OUT_OF_MEMORY);
    case 0xdd76: as_error(ERR_TYPE_MISMATCH);
    case 0x00b1: case 0x00b7: return false;  // CHRGET runs as code
    case 0xdd67: basic_in(); frmnum(); basic_out(); break;
    case 0xdd7b: basic_in(); frmevl(); basic_out(); break;
    case 0xdd6a: basic_in(); if (fac_string) as_error(ERR_TYPE_MISMATCH); break;
    case 0xdd6c: basic_in(); if (!fac_string) as_error(ERR_TYPE_MISMATCH); break;
    case 0xe6f8: basic_in(); cpu.x = (uint8_t)getbyt(); basic_out(); cpu.a = M[tp]; break;
    case 0xe6f5: basic_in(); cpu.x = (uint8_t)gtbytc(); basic_out(); cpu.a = M[tp]; break;
    case 0xe74c: basic_in(); chkcom(); cpu.x = (uint8_t)getbyt(); basic_out(); break;
    case 0xe752: basic_in(); sw16(Z_LINNUM, fp_to_addr(&fac)); basic_out();
        cpu.a = M[Z_LINNUM]; cpu.y = M[Z_LINNUM + 1]; break;
    case 0xe746: basic_in(); sw16(Z_LINNUM, getadr()); chkcom(); cpu.x = (uint8_t)getbyt();
        basic_out(); break;
    case 0xdebe: basic_in(); chkcom(); basic_out(); cpu.a = M[tp]; break;
    case 0xdebb: basic_in(); synchr('('); basic_out(); cpu.a = M[tp]; break;
    case 0xdeb8: basic_in(); synchr(')'); basic_out(); cpu.a = M[tp]; break;
    case 0xdec0: basic_in(); synchr(cpu.a); basic_out(); cpu.a = M[tp]; break;
    case 0xdafb: as_crdo(); break;
    case 0xdb5c: as_outdo(cpu.a); break;
    case 0xdb57: as_outdo(' '); break;
    case 0xdb5a: as_outdo('?'); break;
    case 0xdb3a: {
        uint16_t p = (uint16_t)(cpu.a | cpu.y << 8);
        while (M[p])
            as_outdo(M[p++]);
        break;
    }
    case 0xeaf9: fac = fp_unpack(&M[(uint16_t)(cpu.a | cpu.y << 8)]);
        fac_string = false; as_sync_out(); break;
    case 0xeb2b: basic_in(); fp_pack(&fac, &M[(uint16_t)(cpu.x | cpu.y << 8)]);
        as_sync_out(); break;
    case 0xed34: {
        basic_in();
        char buf[20];
        fp_format(&fac, buf);
        strcpy((char *)&M[0x100], buf);
        as_sync_out();
        cpu.a = 0;
        cpu.y = 1;
        break;
    }
    case 0xed24: {
        as_fp f = fp_from_int16(0);
        unsigned v = cpu.x | cpu.a << 8;
        f.exp = v ? 0x90 : 0;
        f.mant = (uint32_t)v << 16;
        while (v && !(f.mant & 0x80000000u)) {
            f.mant <<= 1;
            f.exp--;
        }
        char buf[20];
        fp_format(&f, buf);
        for (char *s = buf; *s; ++s)
            as_outdo((uint8_t)*s);
        break;
    }
    case 0xe2f2: fac = fp_from_int16((int16_t)(cpu.a << 8 | cpu.y)); fac_string = false;
        as_sync_out(); break;
    case 0xe301: fac = fp_from_uint8(cpu.y); fac_string = false; as_sync_out(); break;
    case 0xe10c: {
        basic_in();
        int v = fp_to_int16(&fac);
        M[0xa0] = (uint8_t)(v >> 8);
        M[0xa1] = (uint8_t)v;
        break;
    }
    // Applesoft: floating point, with the FAC and ARG in zero page
    case 0xe7be: fp_mem_op(fp_add); break;
    case 0xe7a7: fp_mem_op(fp_sub); break;
    case 0xe97f: fp_mem_op(fp_mul); break;
    case 0xea66: fp_mem_op(fp_div); break;
    case 0xe7c1: fp_arg_op(fp_add); break;
    case 0xe7aa: fp_arg_op(fp_sub); break;
    case 0xe982: fp_arg_op(fp_mul); break;
    case 0xea69: fp_arg_op(fp_div); break;
    case 0xee97: fp_arg_op(fp_pow); break;
    case 0xe7a0: { static const uint8_t half[5] = {0x80, 0, 0, 0, 0};
        fac_in(); as_fp a = fp_unpack(half); fp_add(&fac, &a);
        fac_out(); break; }
    case 0xee8d: fp_fn(fp_sqr); break;
    case 0xe941: fp_fn(fp_log); break;
    case 0xef09: fp_fn(fp_exp); break;
    case 0xefea: fp_fn(fp_cos); break;
    case 0xeff1: fp_fn(fp_sin); break;
    case 0xf03a: fp_fn(fp_tan); break;
    case 0xf09e: fp_fn(fp_atn); break;
    case 0xefae: fp_fn(fp_rnd_trap); break;
    case 0xec23: fp_fn(fp_int); break;
    case 0xebf2: fp_fn(fp_qint); break;
    case 0xeed0: fp_fn(fp_neg); break;
    case 0xeb90: fp_fn(fp_sgn_trap); break;
    case 0xebaf: fac_in(); fac.neg = false; fac_out(); break;
    case 0xeb82: fac_in(); cpu.a = (uint8_t)fp_sign(&fac); break;
    case 0xebb2: fac_in(); cpu.a = (uint8_t)fp_compare(&fac, &M[(uint16_t)(cpu.a | cpu.y << 8)]);
        break;
    case 0xeb63: case 0xeb66: {  // FAC to ARG (rounded)
        fac_in();
        if (pc == 0xeb63)
            fp_round(&fac);
        fac.ext = 0;
        fac_out();
        memcpy(&M[Z_ARG], &M[Z_FAC], 6);
        break;
    }
    case 0xeb53: memcpy(&M[Z_FAC], &M[Z_ARG], 6); M[0xac] = 0; break;
    case 0xe9e3: {  // ARG from memory
        as_fp a = fp_unpack(&M[(uint16_t)(cpu.a | cpu.y << 8)]);
        M[Z_ARG] = a.exp;
        M[Z_ARG + 1] = (uint8_t)(a.mant >> 24);
        M[Z_ARG + 2] = (uint8_t)(a.mant >> 16);
        M[Z_ARG + 3] = (uint8_t)(a.mant >> 8);
        M[Z_ARG + 4] = (uint8_t)a.mant;
        M[Z_ARG + 5] = a.neg ? 0xff : 0;
        M[0xab] = M[Z_ARG + 5] ^ M[Z_FAC + 5];
        cpu.a = M[Z_FAC];
        break;
    }
    // Applesoft: the interpreter, more
    case 0xd683: extern void as_stack_init(void); as_stack_init(); break;
    case 0xdfe3: {
        basic_in();
        extern uint16_t as_ptrget(void);
        uint16_t v = as_ptrget();
        basic_out();
        cpu.a = (uint8_t)v;
        cpu.y = (uint8_t)(v >> 8);
        break;
    }
    case 0xda0c: basic_in(); linget(); basic_out(); break;
    case 0xd995: case 0xd9a6: {
        basic_in();
        extern void as_skip_statement(bool whole_line);
        as_skip_statement(pc == 0xd9a6);
        basic_out();
        break;
    }
    case 0xd52c: cpu.x = (uint8_t)as_inlin(0x80); cpu.y = 2; cpu.x = 0xff; cpu.y = 1; break;
    case 0xd52e: as_inlin(cpu.x); cpu.x = 0xff; cpu.y = 1; break;
    // Monitor, more
    case 0xfb2f: as_text_mode(); as_pr(0); break;
    case 0xfc70: as_cout(0x8a); break;  // SCROLL (near enough: a line feed at the bottom)
    case 0xfc1a: if (M[Z_WNDTOP] < M[Z_CV]) { M[Z_CV]--; as_vtab(); } break;
    case 0xfb60: as_home(); { const char *t = "Apple //e"; M[Z_CH] = 15;
        while (*t) as_cout((uint8_t)(*t++ | 0x80)); } break;
    case 0xff2d: as_cout(0xc5); as_cout(0xd2); as_cout(0xd2); as_bell(); break;
    case 0xfe93: as_pr(0); break;
    case 0xfe89: as_in(0); break;
    case 0xfd6f: cpu.x = (uint8_t)as_getln(); break;
    case 0xfd21: cpu.a = as_rdkey(); break;
    case 0xd7d2: case 0xd7d4: case 0x03d0: return false;  // handled by the caller
    default:
        return false;
    }
    return true;
}

/// Runs machine code from pc until it returns to RETURN_TRAP.
static void run_6502(uint16_t pc, bool keep_regs) {
    bool outer = cpu_running;
    as_cpu saved = cpu;
    if (!outer) {
        cpu_base_us = asp_micros();
        cpu.read = cpu_read;
        cpu.write = cpu_write;
        cpu.ctx = NULL;
        cpu_start_cycles = cpu.cycles;
    }
    if (!keep_regs) {
        cpu.a = M[0x45];
        cpu.x = M[0x46];
        cpu.y = M[0x47];
        cpu.p = AS_U;
        cpu.s = 0xf8;
    }
    // Push the return address (RETURN_TRAP - 1).
    M[0x100 | cpu.s] = (RETURN_TRAP - 1) >> 8;
    cpu.s--;
    M[0x100 | cpu.s] = (RETURN_TRAP - 1) & 0xff;
    cpu.s--;
    cpu.pc = pc;
    cpu_running = true;
    uint32_t budget = 0;
    uint64_t next_check = cpu.cycles + 20000;
    for (;;) {
        uint16_t p = cpu.pc;
        if (p == RETURN_TRAP)
            break;
        if (p >= 0xc000 || p == 0x3d0 || p == 0xd7d2) {
            if (p == 0xd7d2 || p == 0xd7d4 || p == 0x3d0 || p == 0x3d3) {
                // NEWSTT or DOS warm start: back to BASIC.
                break;
            }
            if (!rom_trap(p)) {
                cpu_running = outer;
                if (!outer)
                    cpu = saved;
                extern void as_unsupported_call(uint16_t addr);
                as_unsupported_call(p);
            }
            rts();
            continue;
        }
        as_cpu_step(&cpu);
        if (++budget >= 4096) {
            budget = 0;
            // Keep to the Apple's speed (so delay loops and tones work), and
            // let Ctrl-C get through.
            if (cpu.cycles >= next_check) {
                next_check = cpu.cycles + 20000;
                uint64_t due = cpu_base_us + (uint64_t)(cpu.cycles - cpu_start_cycles) * 1000 / 1023;
                uint64_t now = asp_micros();
                if (due > now + 2000)
                    asp_sleep_us((uint32_t)(due - now));
                video_update(false);
            }
        }
    }
    M[0x45] = cpu.a;
    M[0x46] = cpu.x;
    M[0x47] = cpu.y;
    M[0x48] = cpu.p;
    cpu_running = outer;
}

void as_call(uint16_t addr) {
    if (addr >= 0xc000 || addr == 0x3d0) {
        if (addr == 0x3d0 || addr == 0x3d3 || addr == 0xd7d2)
            return;
        as_sync_out();
        cpu.a = M[0x45];
        cpu.x = M[0x46];
        cpu.y = M[0x47];
        if (rom_trap(addr)) {
            as_sync_in();
            return;
        }
        extern void as_unsupported_call(uint16_t addr);
        as_unsupported_call(addr);
    }
    as_sync_out();
    run_6502(addr, false);
    as_sync_in();
}

void as_usr(void) {
    as_sync_out();
    run_6502(0x000a, false);
    as_sync_in();
    if (fac_string)
        as_error(ERR_TYPE_MISMATCH);
}

void as_ampersand(void) {
    run_6502(0x03f5, false);
}

void mach_init(void) {
    // The ROM area reads as zeros except for a few identifying bytes.
    memset(&M[0xd000], 0, 0x3000);
    M[0xfbb3] = 0x06;  // Apple IIe
    M[0xfbc0] = 0xe0;  // enhanced
    M[0xfbbf] = 0x00;
    sw16(0xfffc, 0xfa62);
    sw16(0xfffe, 0xfa40);
    memset(&cpu, 0, sizeof cpu);
    cpu.read = cpu_read;
    cpu.write = cpu_write;
}
