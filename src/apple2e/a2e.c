/**
 * @file a2e.c
 * @brief The Apple IIe's memory map and I/O page: the MMU (main/aux RAM, the language card, the internal slot firmware) and the IOU (display switches), keyboard, speaker and game port.
 *
 * Each memory switch rebuilds the CPU's page tables, so ordinary reads and
 * writes are one table look-up.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */

#include "a2e.h"

#include <string.h>

// On the board the bus runs from SRAM, away from the XIP cache that flash
// code and the PSRAM share.
#if defined(PICO_ON_DEVICE) && PICO_ON_DEVICE
#include "pico.h"
#define FAST(f) __attribute__((section("app_ov_apple2." #f))) f  ///< the apps' SRAM area (app_overlay.h)
#else
#define FAST(f) f
#endif

// ---- The page tables --------------------------------------------------------

static void remap(a2e *m) {
    uint8_t *zp = m->altzp ? m->aux : m->main;
    for (int p = 0; p < 2; ++p)
        m->rd[p] = m->wr[p] = zp + p * 256;

    uint8_t *rd = m->ramrd ? m->aux : m->main;
    uint8_t *wr = m->ramwrt ? m->aux : m->main;
    for (int p = 2; p < 0xc0; ++p) {
        m->rd[p] = rd + p * 256;
        m->wr[p] = wr + p * 256;
    }
    // 80STORE: PAGE2 picks main or aux for the text page (and the hi-res
    // page with HIRES), whatever RAMRD and RAMWRT say.
    if (m->store80) {
        uint8_t *bank = m->page2 ? m->aux : m->main;
        for (int p = 0x04; p < 0x08; ++p)
            m->rd[p] = m->wr[p] = bank + p * 256;
        if (m->hires)
            for (int p = 0x20; p < 0x40; ++p)
                m->rd[p] = m->wr[p] = bank + p * 256;
    }

    for (int p = 0xc0; p < 0xd0; ++p) {
        m->rd[p] = NULL;
        m->wr[p] = NULL;
    }

    // The language card: RAM from the bank ALTZP picks, or the ROM.
    uint8_t *lc = m->altzp ? m->aux : m->main;
    uint8_t *lc1 = m->altzp ? m->lc1_aux : m->lc1_main;
    for (int p = 0xd0; p < 0x100; ++p) {
        uint8_t *ram = (p < 0xe0 && !m->lc_bank2) ? lc1 + (p - 0xd0) * 256 : lc + p * 256;
        m->rd[p] = m->lc_read_ram ? ram : m->rom + (p - 0xc0) * 256;
        m->wr[p] = m->lc_write_ram ? ram : NULL;
    }
}

// ---- $C100-$CFFF: slot ROMs and the internal firmware ---------------------------

/// What the bus holds when nothing drives it. A real IIe shows the byte the
/// video circuits last fetched; nothing here depends on that yet.
#define FLOATING 0xa0

static uint8_t FAST(cx_access)(a2e *m, uint16_t a, bool side_effects, bool write, uint8_t wv) {
    unsigned page = a >> 8;
    uint8_t v;
    unsigned slot = page & 7;
    if (side_effects && !m->intcxrom && page < 0xc8 && m->slot_mem[slot] &&
        (slot != 3 || m->slotc3rom))
        return m->slot_mem[slot](m->slot_ctx[slot], (uint8_t)a, write, wv);
    if (m->intcxrom)
        v = m->rom[a - 0xc000];
    else if (page == 0xc3)
        v = m->slotc3rom && (m->slot_has_rom & 8) ? m->slot_rom[3][a & 0xff]
            : m->slotc3rom                       ? FLOATING
                                                 : m->rom[a - 0xc000];
    else if (page >= 0xc8)
        v = m->intc8rom ? m->rom[a - 0xc000] : FLOATING;  // no card has expansion ROM yet
    else
        v = (m->slot_has_rom & (1u << (page & 7))) ? m->slot_rom[page & 7][a & 0xff] : FLOATING;

    if (side_effects) {
        // Using the 80-column firmware in $C3xx switches its $C800 part in;
        // touching $CFFF switches every $C800 ROM out.
        if (page == 0xc3 && !m->slotc3rom)
            m->intc8rom = true;
        if (a == 0xcfff)
            m->intc8rom = false;
    }
    return v;
}

// ---- The I/O page ------------------------------------------------------------

static void lc_switch(a2e *m, uint8_t r, bool write) {
    // $C080-$C08F: bit 3 picks bank 1, bits 0-1 what reads and writes reach.
    // Writing to RAM needs two reads in a row of an odd address.
    m->lc_bank2 = !(r & 8);
    m->lc_read_ram = (r & 3) == 0 || (r & 3) == 3;
    if (r & 1) {
        if (write) {
            m->lc_prewrite = false;
        } else {
            if (m->lc_prewrite)
                m->lc_write_ram = true;
            m->lc_prewrite = true;
        }
    } else {
        m->lc_write_ram = false;
        m->lc_prewrite = false;
    }
    remap(m);
}

static uint8_t FAST(io)(a2e *m, uint16_t a, bool write, uint8_t v) {
    uint8_t r = a & 0xff;
    uint8_t kbd = m->key | (m->strobe ? 0x80 : 0);

    if (r < 0x10) {
        // $C000-$C00F: reads give the keyboard; writes set the MMU and IOU.
        if (!write)
            return kbd;
        bool on = r & 1;
        switch (r >> 1) {
        case 0: m->store80 = on; break;
        case 1: m->ramrd = on; break;
        case 2: m->ramwrt = on; break;
        case 3: m->intcxrom = on; break;
        case 4: m->altzp = on; break;
        case 5: m->slotc3rom = on; break;
        case 6: m->col80 = on; return 0;
        case 7: m->altchar = on; return 0;
        }
        remap(m);
        return 0;
    }

    if (r < 0x20) {
        // $C010 clears the strobe (writes anywhere in $C010-$C01F do too);
        // $C011-$C01F read a switch in bit 7 over the key's low 7 bits.
        if (write || r == 0x10) {
            m->strobe = false;
            return write ? 0 : (uint8_t)(m->key | (m->key_down ? 0x80 : 0));
        }
        bool flag;
        switch (r) {
        case 0x11: flag = m->lc_bank2; break;
        case 0x12: flag = m->lc_read_ram; break;
        case 0x13: flag = m->ramrd; break;
        case 0x14: flag = m->ramwrt; break;
        case 0x15: flag = m->intcxrom; break;
        case 0x16: flag = m->altzp; break;
        case 0x17: flag = m->slotc3rom; break;
        case 0x18: flag = m->store80; break;
        case 0x19: flag = !a2e_in_vbl(m); break;
        case 0x1a: flag = m->text; break;
        case 0x1b: flag = m->mixed; break;
        case 0x1c: flag = m->page2; break;
        case 0x1d: flag = m->hires; break;
        case 0x1e: flag = m->altchar; break;
        default:   flag = m->col80; break;
        }
        return (uint8_t)((flag ? 0x80 : 0) | m->key);
    }

    if (r < 0x30)  // cassette output
        return FLOATING;

    if (r < 0x40) {
        m->speaker_clicks++;
        if (m->speaker)
            m->speaker(m->user, m->cycles);
        return FLOATING;
    }

    if (r < 0x50)  // game port strobe
        return FLOATING;

    if (r < 0x60) {
        bool on = r & 1;
        switch (r) {
        case 0x50: case 0x51: m->text = on; break;
        case 0x52: case 0x53: m->mixed = on; break;
        case 0x54: case 0x55:
            m->page2 = on;
            if (m->store80)
                remap(m);
            break;
        case 0x56: case 0x57:
            m->hires = on;
            if (m->store80)
                remap(m);
            break;
        default:
            // Annunciators; AN3 off ($C05E) is double hi-res on.
            m->an[(r - 0x58) >> 1] = on;
            if (r >= 0x5e)
                m->dhires = !on;
            break;
        }
        return FLOATING;
    }

    if (r < 0x70) {
        // $C060-$C06F (the top half mirrors the bottom): cassette in, the
        // buttons, the paddle timers. Bit 7 only.
        uint8_t hi;
        switch (r & 7) {
        case 0: hi = 0; break;
        case 1: case 2: case 3: hi = m->button[(r & 7) - 1] ? 0x80 : 0; break;
        default: {
            // A paddle's timer runs about 11 cycles per unit after $C070.
            uint64_t since = m->cycles - m->paddle_trigger;
            hi = since < (uint64_t)m->paddle[(r & 7) - 4] * 11 ? 0x80 : 0;
            break;
        }
        }
        return (uint8_t)(hi | (FLOATING & 0x7f));
    }

    if (r < 0x80) {
        // $C070 starts the paddle timers; on the enhanced IIe $C07E/$C07F
        // also read IOUDIS and DHIRES, and writes set and clear IOUDIS.
        m->paddle_trigger = m->cycles;
        if (r == 0x7e) {
            if (write)
                m->ioudis = true;
            else
                return (uint8_t)((m->ioudis ? 0x80 : 0) | (FLOATING & 0x7f));
        } else if (r == 0x7f) {
            if (write)
                m->ioudis = false;
            else
                return (uint8_t)((m->dhires ? 0x80 : 0) | (FLOATING & 0x7f));
        }
        return FLOATING;
    }

    if (r < 0x90) {
        lc_switch(m, r & 0x0f, write);
        return FLOATING;
    }

    unsigned slot = (r >> 4) - 8;
    if (m->slot_io[slot])
        return m->slot_io[slot](m->slot_ctx[slot], r & 0x0f, write, v);
    return FLOATING;
}

// ---- The CPU's bus -------------------------------------------------------------

static uint8_t FAST(cpu_read)(void *ctx, uint16_t a) {
    a2e *m = ctx;
    const uint8_t *p = m->rd[a >> 8];
    if (p)
        return p[a & 0xff];
    return a < 0xc100 ? io(m, a, false, 0) : cx_access(m, a, true, false, 0);
}

static void FAST(cpu_write)(void *ctx, uint16_t a, uint8_t v) {
    a2e *m = ctx;
    uint8_t *p = m->wr[a >> 8];
    if (p)
        p[a & 0xff] = v;
    else if (a < 0xc100)
        io(m, a, true, v);
    else if (a < 0xd000)
        cx_access(m, a, true, true, v);
}

// ---- The interface ---------------------------------------------------------------

void a2e_init(a2e *m, const uint8_t rom16k[0x4000]) {
    memcpy(m->rom, rom16k, sizeof m->rom);
    m->cpu.read = cpu_read;
    m->cpu.write = cpu_write;
    m->cpu.ctx = m;
    m->text = true;
    for (int s = 0; s < 8; ++s)
        m->slot_event_at[s] = UINT64_MAX;
    m->next_event = UINT64_MAX;
    a2e_reset(m);
}

void a2e_reset(a2e *m) {
    // The reset line clears the MMU: main memory, slot ROMs, and the
    // language card reading ROM and writing bank 2.
    m->store80 = m->ramrd = m->ramwrt = m->intcxrom = m->altzp = m->slotc3rom = false;
    m->intc8rom = false;
    m->lc_bank2 = true;
    m->lc_read_ram = false;
    m->lc_write_ram = true;
    m->lc_prewrite = false;
    m->ioudis = false;
    remap(m);
    for (int s = 0; s < 8; ++s)
        if (m->slot_reset[s])
            m->slot_reset[s](m->slot_ctx[s]);
    as_cpu_reset(&m->cpu);
}

static void run_events(a2e *m) {
    for (int s = 0; s < 8; ++s)
        if (m->slot_event_at[s] <= m->cycles) {
            uint64_t at = m->slot_event_at[s];
            m->slot_event_at[s] = UINT64_MAX;
            m->slot_event[s](m->slot_ctx[s], at);  // may ask for the next one
        }
    uint64_t next = UINT64_MAX;
    for (int s = 0; s < 8; ++s)
        if (m->slot_event_at[s] < next)
            next = m->slot_event_at[s];
    m->next_event = next;
}

void a2e_cold_start(a2e *m) {
    m->main[0x3f4] = (uint8_t)(m->main[0x3f3] ^ 0xa5 ^ 0xff);
    a2e_reset(m);
}

uint32_t FAST(a2e_run)(a2e *m, uint32_t cycles) {
    uint32_t done = 0;
    while (done < cycles) {
        int c = as_cpu_step(&m->cpu);
        done += (uint32_t)c;
        m->cycles += (uint64_t)c;
        if (m->cycles >= m->next_event)
            run_events(m);
    }
    return done;
}

void a2e_set_event(a2e *m, int slot, uint64_t at) {
    m->slot_event_at[slot] = at;
    if (at < m->next_event)
        m->next_event = at;
}

void a2e_set_irq(a2e *m, int slot, bool on) {
    if (on)
        m->irq_slots |= (uint8_t)(1u << slot);
    else
        m->irq_slots &= (uint8_t)~(1u << slot);
    m->cpu.irq = m->irq_slots != 0;
}

void a2e_key(a2e *m, uint8_t ascii) {
    m->key = ascii & 0x7f;
    m->strobe = true;
}

void a2e_key_held(a2e *m, bool down) {
    m->key_down = down;
}

uint8_t a2e_peek(const a2e *m, uint16_t a) {
    const uint8_t *p = m->rd[a >> 8];
    if (p)
        return p[a & 0xff];
    if (a < 0xc100)
        return 0;
    return cx_access((a2e *)m, a, false, false, 0);
}
