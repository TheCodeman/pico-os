/**
 * @file c64.c
 * @brief Implementation of c64.h: a PAL Commodore 64.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */

#include "c64.h"

#include "c64_cart.h"
#include "c64_drive.h"

#include <string.h>

#if defined(PICO_ON_DEVICE) && PICO_ON_DEVICE
#include "pico.h"
#define FAST(f) __attribute__((section("app_ov_c64." #f))) f  ///< the apps' SRAM area (app_overlay.h)
#else
#define FAST(f) f
#endif

#define TOD_CYCLES (C64_CPU_HZ / 10)

// ---- Memory ------------------------------------------------------------------

static uint8_t port_value(c64 *m) {
    // Inputs: bits 0-2 pulled up, bit 4 the cassette sense (no button
    // pressed), and bits 6 and 7 not connected (they keep what was written).
    uint8_t in = 0x17 | (m->port_data & 0xc0);
    return (m->port_data & m->port_dir) | (in & ~m->port_dir);
}

static uint8_t port_read(c64 *m, uint16_t addr) {
    return addr ? port_value(m) : m->port_dir;
}

/// The PLA: LORAM, HIRAM and CHAREN are the port's bits 0-2, pulled high
/// when they are inputs; a cartridge's EXROM and GAME lines (low: on) bring
/// in its ROML at $8000 and ROMH at $A000 (16 KB) or $E000 (Ultimax, where
/// most of memory is left open and the CPU sees only 4 KB of RAM).
static void map_memory(c64 *m) {
    uint8_t lines = (uint8_t)(m->port_data | ~m->port_dir) & 7;
    bool loram = lines & 1, hiram = lines & 2, charen = lines & 4;
    const c64_cart *c = m->cart;
    bool exrom = c ? c->exrom : true, game = c ? c->game : true;
    const uint8_t *roml = c ? c->lo[c->bank] : NULL, *romh = c ? c->hi[c->bank] : NULL;
    for (int p = 0; p < 256; ++p) {
        m->rd[p] = m->ram + p * 256;
        m->wr[p] = m->ram + p * 256;
    }
    if (!game && exrom) {  // Ultimax
        for (int p = 0; p < 0x20; ++p)
            if (roml)
                m->rd[0x80 + p] = roml + p * 256;
        for (int p = 0; p < 0x20; ++p)
            if (romh)
                m->rd[0xe0 + p] = romh + p * 256;
        for (int p = 0xd0; p < 0xe0; ++p)
            m->rd[p] = m->wr[p] = NULL;
        m->io_in = true;
    } else {
        bool cart16 = !exrom && !game;
        if (!exrom && loram && hiram && roml)
            for (int p = 0; p < 0x20; ++p)
                m->rd[0x80 + p] = roml + p * 256;
        if (cart16 && hiram && romh) {
            for (int p = 0; p < 0x20; ++p)
                m->rd[0xa0 + p] = romh + p * 256;
        } else if (loram && hiram) {
            for (int p = 0xa0; p < 0xc0; ++p)
                m->rd[p] = m->basic + (p - 0xa0) * 256;
        }
        if (hiram)
            for (int p = 0xe0; p < 0x100; ++p)
                m->rd[p] = m->kernal + (p - 0xe0) * 256;
        m->io_in = (loram || hiram) && charen;
        if (loram || hiram)
            for (int p = 0xd0; p < 0xe0; ++p) {
                m->rd[p] = charen ? NULL : m->charrom + (p - 0xd0) * 256;
                if (charen)
                    m->wr[p] = NULL;
            }
    }
    // What reading the port gives, where zero page reads find it.
    m->ram[0] = m->port_dir;
    m->ram[1] = port_value(m);
}


/// CIA 1: the keyboard and joysticks. A key joins its port A line to its
/// port B line, so a column driven low pulls down the rows of its pressed keys
/// and the other way round.
static uint8_t cia1_port(void *ctx, c64_cia *cia, int port) {
    c64 *m = ctx;
    uint8_t a = c64_cia_out(cia, 0) & ~m->joy[1], b = c64_cia_out(cia, 1) & ~m->joy[0];
    if (port) {
        for (int i = 0; i < 8; ++i)
            if (!(a & (1 << i)))
                b &= ~m->keys[i];
        return b;
    }
    for (int i = 0; i < 8; ++i)
        if (m->keys[i] & ~b)
            a &= ~(1 << i);
    return a;
}

/// CIA 2: port A bits 0-1 choose the VIC's bank (inverted), bits 3-5 drive
/// the serial bus's ATN, CLOCK and DATA through inverters, and bits 6-7 read
/// CLOCK and DATA back, as this and a 1541 (if there is one) pull them.
static uint8_t cia2_port(void *ctx, c64_cia *cia, int port) {
    c64 *m = ctx;
    uint8_t out = c64_cia_out(cia, port);
    if (port)
        return out;
    bool clk_low = out & 0x10, data_low = out & 0x20;
    if (m->drive) {
        bool dc, dd;
        c64_drive_bus(m->drive, &dc, &dd);
        clk_low |= dc;
        data_low |= dd;
    }
    return (out & 0x3f) | (clk_low ? 0 : 0x40) | (data_low ? 0 : 0x80);
}

static void update_vic_bank(c64 *m) {
    m->vic.bank = (uint16_t)((~c64_cia_out(&m->cia2, 0) & 3) << 14);
}

static void sid_v3_catch_up(c64 *m) {
    c64_sid_run_voice3(&m->sid_v3, (uint32_t)(m->cycles - m->sid_v3_cycles));
    m->sid_v3_cycles = m->cycles;
}

static uint8_t sid_read(c64 *m, uint8_t reg) {
    switch (reg) {
    case 0x19: case 0x1a:
        return 0xff;  // paddles: none
    case 0x1b:
        sid_v3_catch_up(m);
        return c64_sid_osc3(&m->sid_v3);
    case 0x1c:
        sid_v3_catch_up(m);
        return c64_sid_env3(&m->sid_v3);
    default:
        return 0;
    }
}

static uint8_t FAST(io_read)(c64 *m, uint16_t addr) {
    switch ((addr >> 8) & 0x0f) {
    case 0x0: case 0x1: case 0x2: case 0x3:
        return c64_vic_read(&m->vic, addr & 0x3f);
    case 0x4: case 0x5: case 0x6: case 0x7:
        return sid_read(m, addr & 0x1f);
    case 0x8: case 0x9: case 0xa: case 0xb:
        return m->color[addr & 0x3ff] | 0xf0;
    case 0xc:
        return c64_cia_read(&m->cia1, addr & 0x0f);
    case 0xd:
        // The serial bus: the drive catches up to the read, which comes in
        // an absolute LDA's last (fourth) cycle.
        if (m->drive && !(addr & 0x0f))
            c64_drive_run_to(m->drive, m->cycles + 3);
        return c64_cia_read(&m->cia2, addr & 0x0f);
    default:
        return c64_cart_io_read(m, addr);  // I/O 1 and 2: the cartridge's
    }
}

static void FAST(io_write)(c64 *m, uint16_t addr, uint8_t v) {
    switch ((addr >> 8) & 0x0f) {
    case 0x0: case 0x1: case 0x2: case 0x3:
        // Where the beam is when the write lands, in an absolute STA's last
        // (fourth) cycle.
        m->vic.write_x = (int16_t)c64_vic_cycle_x((int)(m->cycles - m->line_cycle) + 3);
        c64_vic_write(&m->vic, addr & 0x3f, v);
        break;
    case 0x4: case 0x5: case 0x6: case 0x7:
        m->sid[addr & 0x1f] = v;
        if ((addr & 0x1f) >= 0x0e && (addr & 0x1f) <= 0x14) {
            sid_v3_catch_up(m);
            c64_sid_write(&m->sid_v3, addr & 0x1f, v);
        }
        if (m->sid_write)
            m->sid_write(m->user, addr & 0x1f, v, m->cycles);
        break;
    case 0x8: case 0x9: case 0xa: case 0xb:
        m->color[addr & 0x3ff] = v & 0x0f;
        break;
    case 0xc:
        c64_cia_write(&m->cia1, addr & 0x0f, v);
        break;
    case 0xd:
        if (m->drive && (addr & 0x0f) <= 2)
            c64_drive_run_to(m->drive, m->cycles + 3);  // the same for writes
        c64_cia_write(&m->cia2, addr & 0x0f, v);
        update_vic_bank(m);
        break;
    default:
        c64_cart_io_write(m, addr, v);
        break;
    }
}

void c64_attach_drive(c64 *m, struct c64_drive *d) {
    m->drive = d;
    if (d)
        c64_drive_sync(d, m->cycles);
}

void c64_cart_changed(c64 *m) {
    map_memory(m);
}

static uint8_t FAST(cpu_read)(void *ctx, uint16_t addr) {
    c64 *m = ctx;
    if (__builtin_expect(addr < 2, 0))
        return port_read(m, addr);
    const uint8_t *p = m->rd[addr >> 8];
    return p ? p[addr & 0xff] : io_read(m, addr);
}

static void FAST(cpu_write)(void *ctx, uint16_t addr, uint8_t v) {
    c64 *m = ctx;
    if (m->io_in && (addr & 0xf000) == 0xd000) {
        io_write(m, addr, v);
        return;
    }
    if (__builtin_expect(addr < 2, 0)) {
        if (addr)
            m->port_data = v;
        else
            m->port_dir = v;
        map_memory(m);  // which puts the port's values at $0000 and $0001
        return;
    }
    m->ram[addr] = v;  // under ROM a write lands in RAM
}

uint8_t c64_peek(c64 *m, uint16_t addr) {
    return cpu_read(m, addr);
}

void c64_poke(c64 *m, uint16_t addr, uint8_t value) {
    cpu_write(m, addr, value);
}

// ---- Running ---------------------------------------------------------------

void c64_init(c64 *m) {
    m->cpu.read = cpu_read;
    m->cpu.write = cpu_write;
    m->cpu.ctx = m;
    m->cpu.rpage = m->rd;
    m->cpu.wpage = m->wr;
    m->cia1.port_in = cia1_port;
    m->cia1.ctx = m;
    m->cia2.port_in = cia2_port;
    m->cia2.ctx = m;
    m->vic.ram = m->ram;
    m->vic.color = m->color;
    m->vic.charrom = m->charrom;
    c64_reset(m);
}

void c64_reset(c64 *m) {
    m->port_dir = 0;
    m->port_data = 0;
    if (m->cart)
        c64_cart_reset(m->cart);
    if (m->drive)  // the serial bus carries the reset line
        c64_drive_reset(m->drive);
    map_memory(m);
    c64_cia_reset(&m->cia1);
    c64_cia_reset(&m->cia2);
    update_vic_bank(m);
    c64_vic_reset(&m->vic);
    memset(m->sid, 0, sizeof m->sid);
    c64_sid_init(&m->sid_v3, C64_CPU_HZ, 48000);
    m->sid_v3_cycles = m->cycles;
    if (m->sid_write)  // the chip's reset: every register 0
        for (uint8_t r = 0; r < 0x19; ++r)
            m->sid_write(m->user, r, 0, m->cycles);
    m->budget = 0;
    m->nmi_line = false;
    c64_cpu_reset(&m->cpu);
}

void c64_power_on(c64 *m) {
    for (int i = 0; i < 0x10000; ++i)
        m->ram[i] = (i & 0x40) ? 0xff : 0x00;
    memset(m->color, 0, 0x400);
    memset(m->keys, 0, sizeof m->keys);
    m->restore = false;
    m->tod_acc = 0;
    c64_reset(m);  // the cycle count goes on: the caller's timing follows it
}

bool c64_ready(const c64 *m) {
    // $E5CD-$E5D4: LDA $C6 ; STA $CC ; STA $0292 ; BEQ $E5CD, with the
    // keyboard buffer empty.
    return m->cpu.pc >= 0xe5cd && m->cpu.pc <= 0xe5d4 && m->rd[0xe5] == m->kernal + 0x500 &&
           m->ram[0xc6] == 0;
}

void c64_start_prg(c64 *m, const uint8_t *prg, int len) {
    if (len < 3)
        return;
    uint16_t at = prg[0] | prg[1] << 8;
    uint32_t n = (uint32_t)len - 2;
    if (at + n > 0x10000)
        n = 0x10000 - at;
    memcpy(m->ram + at, prg + 2, n);
    if (at != 0x0801)
        return;
    uint16_t end = (uint16_t)(at + n);
    for (int z = 0x2d; z <= 0x31; z += 2) {  // VARTAB, ARYTAB, STREND
        m->ram[z] = end & 0xff;
        m->ram[z + 1] = end >> 8;
    }
    memcpy(m->ram + 0x277, "RUN\r", 4);  // the keyboard buffer
    m->ram[0xc6] = 4;
}

static inline void tick(c64 *m, unsigned n) {
    m->cycles += n;
    if ((m->cia1.cra | m->cia1.crb) & 1)  // only while a timer runs
        c64_cia_tick(&m->cia1, n);
    if ((m->cia2.cra | m->cia2.crb) & 1)
        c64_cia_tick(&m->cia2, n);
    m->tod_acc += n;
    if (m->tod_acc >= TOD_CYCLES) {
        m->tod_acc -= TOD_CYCLES;
        c64_cia_tod_tick(&m->cia1);
        c64_cia_tod_tick(&m->cia2);
    }
}

// ---- LOAD from disk ---------------------------------------------------------

/// The KERNAL's LOAD, at $F4A5 (where the LOAD vector at $0330 points): A is
/// 0 to load or 1 to verify, $BA the device, $B9 the secondary address
/// (0: load at $C3/$C4, BASIC's start; else at the file's own address), $B7
/// and $BB/$BC the name. It returns with the end address in X/Y and $AE/$AF
/// and carry clear, or carry set and the error in A.
#define KERNAL_LOAD 0xf4a5

static void FAST(rts)(c64 *m) {
    uint8_t lo = m->ram[0x100 | (uint8_t)(m->cpu.s + 1)], hi = m->ram[0x100 | (uint8_t)(m->cpu.s + 2)];
    m->cpu.s += 2;
    m->cpu.pc = (uint16_t)((lo | hi << 8) + 1);
}

static void kernal_load(c64 *m) {
    uint8_t *ram = m->ram;
    unsigned device = ram[0xba];
    if (device < 8 || (device == 8 && m->drive))
        return;  // tape, or the 1541: the KERNAL's own
    uint8_t name[256];
    int len = ram[0xb7];
    uint16_t at = ram[0xbb] | ram[0xbc] << 8;
    for (int i = 0; i < len; ++i)
        name[i] = cpu_read(m, (uint16_t)(at + i));
    int n = m->load ? m->load(m->user, device, name, len, m->load_buf) : C64_LOAD_NO_DEVICE;
    if (n < 2) {
        m->cpu.a = (uint8_t)(n < 0 ? -n : 4);
        m->cpu.p |= C64_C;
        ram[0x90] = n == C64_LOAD_NO_DEVICE ? 0x80 : 0x42;  // the status: no device / not found
        rts(m);
        return;
    }
    const uint8_t *buf = m->load_buf;
    uint16_t start = ram[0xb9] ? (uint16_t)(buf[0] | buf[1] << 8) : (uint16_t)(ram[0xc3] | ram[0xc4] << 8);
    uint16_t end = start;
    bool verify = ram[0x93];
    ram[0x90] = 0;
    for (int i = 2; i < n; ++i, ++end) {
        if (verify) {
            if (ram[end] != buf[i])
                ram[0x90] |= 0x10;  // VERIFY ERROR
        } else {
            ram[end] = buf[i];  // into RAM, under the ROMs and I/O too
        }
    }
    ram[0xae] = end & 0xff;
    ram[0xaf] = end >> 8;
    m->cpu.x = end & 0xff;
    m->cpu.y = end >> 8;
    m->cpu.p &= ~C64_C;
    rts(m);
}

/// The KERNAL's SAVE, at $F5ED (where the SAVE vector at $0332 points): the
/// file is $C1/$C2 up to (not including) $AE/$AF; the device and name as for
/// LOAD. It returns with carry clear, or carry set and the error in A.
#define KERNAL_SAVE 0xf5ed

static void kernal_save(c64 *m) {
    uint8_t *ram = m->ram;
    unsigned device = ram[0xba];
    if (device < 8 || (device == 8 && m->drive))
        return;  // tape, or the 1541: the KERNAL's own
    uint8_t name[256];
    int len = ram[0xb7];
    uint16_t at = ram[0xbb] | ram[0xbc] << 8;
    for (int i = 0; i < len; ++i)
        name[i] = cpu_read(m, (uint16_t)(at + i));
    uint16_t start = ram[0xc1] | ram[0xc2] << 8, end = ram[0xae] | ram[0xaf] << 8;
    uint8_t *buf = m->load_buf;
    int n = 0;
    buf[n++] = start & 0xff;
    buf[n++] = start >> 8;
    for (uint32_t a = start; a < end; ++a) {
        const uint8_t *p = m->rd[a >> 8];
        buf[n++] = p ? p[a & 0xff] : ram[a];  // as the KERNAL reads it; RAM under I/O
    }
    int r = m->save && len ? m->save(m->user, device, name, len, buf, n) : C64_LOAD_NO_DEVICE;
    if (r < 0) {
        m->cpu.a = (uint8_t)-r;
        m->cpu.p |= C64_C;
        ram[0x90] = r == C64_LOAD_NO_DEVICE ? 0x80 : 0x03;
    } else {
        m->cpu.p &= ~C64_C;
        ram[0x90] = 0;
    }
    rts(m);
}

/// The KERNAL's RAMTAS, at $FD50: clears pages 0 (from $02), 2 and 3, sets
/// the tape buffer's pointer, finds the top of memory by testing each byte
/// from $0400 up (writing $55 and $AA and putting it back) until one isn't
/// RAM, and sets BASIC's memory bottom ($0800) and the screen ($0400). The
/// test takes some 2 seconds; with fast_boot the same comes out at once.
#define KERNAL_RAMTAS 0xfd50

static void kernal_ramtas(c64 *m) {
    uint8_t *ram = m->ram;
    memset(ram + 2, 0, 0xfe);
    memset(ram + 0x200, 0, 0x200);
    ram[0xb2] = 0x3c;  // the tape buffer: $033C
    ram[0xb3] = 0x03;
    int p = 4;
    while (p < 0x100 && m->rd[p] == ram + p * 256 && m->wr[p] == ram + p * 256)
        ++p;
    p &= 0xff;
    ram[0xc1] = 0;
    ram[0xc2] = (uint8_t)p;
    ram[0x283] = 0;  // MEMTOP
    ram[0x284] = (uint8_t)p;
    ram[0x282] = 0x08;  // MEMBOT $0800
    ram[0x288] = 0x04;  // the screen at $0400
    m->cpu.a = 0x04;
    m->cpu.x = 0;
    m->cpu.y = (uint8_t)p;
    m->cpu.p &= ~C64_C;
    rts(m);
}

static void FAST(run_cpu)(c64 *m, int cycles) {
    m->budget += cycles;
    while (m->budget > 0) {
        m->cpu.irq = m->vic.irq || m->cia1.irq;
        bool nmi = m->cia2.irq || m->restore;
        if (nmi && !m->nmi_line)
            m->cpu.nmi = true;
        m->nmi_line = nmi;
        if (__builtin_expect(m->cpu.pc >= 0xf400, 0) && !m->cpu.nmi && !(m->cpu.irq && !(m->cpu.p & C64_I))) {
            if (m->cpu.pc == KERNAL_LOAD && m->rd[0xf4] == m->kernal + 0x1400)
                kernal_load(m);
            else if (m->cpu.pc == KERNAL_SAVE && m->rd[0xf5] == m->kernal + 0x1500)
                kernal_save(m);
            else if (m->cpu.pc == KERNAL_RAMTAS && m->fast_boot && m->rd[0xfd] == m->kernal + 0x1d00)
                kernal_ramtas(m);
        }
        int n = c64_cpu_step(&m->cpu);
        m->budget -= n;
        tick(m, (unsigned)n);
        if (m->drive)  // in step, an instruction at a time
            c64_drive_run_to(m->drive, m->cycles);
    }
}

/// Where in the line the VIC fetches for a bad line.
#define DRAW_CYCLE 14

void FAST(c64_run_line)(c64 *m) {
    c64_vic_line_start(&m->vic);
    m->line_cycle = m->cycles;
    run_cpu(m, DRAW_CYCLE);
    int stolen = c64_vic_line_fetch(&m->vic);
    tick(m, (unsigned)stolen);
    run_cpu(m, C64_LINE_CYCLES - DRAW_CYCLE - stolen);
    uint32_t t0 = m->time_us ? m->time_us() : 0;
    c64_vic_line_finish(&m->vic);  // the line drawn (unless writes drew some of it already)
    if (m->time_us)
        m->video_us += m->time_us() - t0;
    c64_vic_line_end(&m->vic);
}

void c64_run_frame(c64 *m) {
    do
        c64_run_line(m);
    while (m->vic.raster != 0);
}

// ---- Keyboard --------------------------------------------------------------

void c64_key(c64 *m, unsigned key, bool down) {
    if (key >= 64)
        return;
    if (down)
        m->keys[key >> 3] |= 1 << (key & 7);
    else
        m->keys[key >> 3] &= ~(1 << (key & 7));
}

/// The unshifted character of each key in the matrix (0: none).
static const char key_chars[65] =
    "\b\r\0\0\0\0\0\0"   // DEL RETURN CRSR F7 F1 F3 F5 CRSR
    "3wa4zse\0"          // ... LSHIFT
    "5rd6cftx"
    "7yg8bhuv"
    "9ij0mkon"
    "+pl-.:@,"
    "\0*;\0\0=^/"         // POUND * ; HOME RSHIFT = UPARROW /
    "1_\0002 \0q\0";     // 1 LEFTARROW CTRL 2 SPACE CBM Q RUNSTOP

/// Shifted characters and the key they are on.
static const char shifted_from[] = "!\"#$%&'()[]<>?";
static const char shifted_on[]   = "123456789:;,./";

bool c64_ascii_key(char ch, unsigned *key, bool *shift) {
    if (ch >= 'A' && ch <= 'Z')
        ch = (char)(ch - 'A' + 'a');
    const char *s = memchr(shifted_from, ch, sizeof shifted_from - 1);
    *shift = s != NULL;
    if (s)
        ch = shifted_on[s - shifted_from];
    if (ch == '\n')
        ch = '\r';
    if (!ch)
        return false;
    for (unsigned k = 0; k < 64; ++k)
        if (key_chars[k] == ch) {
            *key = k;
            return true;
        }
    return false;
}

char c64_screen_ascii(uint8_t code) {
    code &= 0x7f;
    if (code == 0)
        return '@';
    if (code <= 26)
        return (char)('A' + code - 1);
    if (code < 32)
        return "[\\]^<"[code - 27];
    if (code < 64)
        return (char)code;
    return code == 0x60 ? ' ' : '#';
}
