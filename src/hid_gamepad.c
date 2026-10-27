/**
 * @file hid_gamepad.c
 * @brief A small HID report descriptor parser: just enough to find a joystick's axes, hat switch and buttons (HID 1.11, section 6.2.2).
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */

#include "hid_gamepad.h"

#include <string.h>

/// HID usage pages and usages the parser looks for.
enum {
    PAGE_DESKTOP = 0x01, PAGE_BUTTON = 0x09,
    USAGE_X = 0x30, USAGE_Y = 0x31, USAGE_HAT = 0x39,
};

/// Usages remembered between Main items.
#define MAX_USAGES 16
/// Report IDs the bit counts are kept for.
#define MAX_IDS    16

/// The global items in force while the report descriptor is read.
typedef struct {
    uint16_t page;  ///< the usage page
    /// the logical minimum and maximum
    ///@{
    int32_t min, max;
    ///@}
    uint32_t max_raw;  ///< the maximum's bytes as unsigned
    /// the report size, count and ID
    ///@{
    uint32_t size, count, id;
    ///@}
} globals;

/// The bits used so far in each numbered report.
typedef struct {
    uint8_t id[MAX_IDS];  ///< report IDs seen
    uint16_t bits[MAX_IDS];  ///< bits so far in each
    int n;  ///< how many IDs are used
} report_bits;

/// The bit count of a report ID, adding it if it's new.
/// @param r the counts
/// @param id the report ID
/// @return its count, or NULL if the table is full
static uint16_t *bits_for(report_bits *r, uint8_t id) {
    for (int i = 0; i < r->n; ++i)
        if (r->id[i] == id)
            return &r->bits[i];
    if (r->n == MAX_IDS)
        return NULL;
    r->id[r->n] = id;
    r->bits[r->n] = 0;
    return &r->bits[r->n++];
}

/// Makes a field from the global items, at a bit position.
/// @param f the field
/// @param bit its first bit
/// @param g the globals in force
static void set_field(hid_field *f, uint32_t bit, const globals *g) {
    f->bit = (uint16_t)bit;
    f->size = (uint16_t)g->size;
    f->min = g->min;
    f->max = g->max;
    f->is_signed = g->min < 0;
    f->valid = true;
}

bool hid_gamepad_parse(const uint8_t *desc, size_t len, hid_gamepad_layout *out) {
    hid_gamepad_layout lay;
    memset(&lay, 0, sizeof lay);
    globals g = {0}, stack[4];
    int depth = 0;
    uint32_t usages[MAX_USAGES];  // page << 16 | usage
    int nusages = 0;
    uint32_t umin = 0, umax = 0;
    bool range = false, have_id = false;
    uint8_t button_id = 0;
    report_bits rb = {0};

    for (size_t i = 0; i < len;) {
        uint8_t b = desc[i++];
        if (b == 0xfe) {  // long item: skip it
            if (i + 1 >= len)
                break;
            i += 2 + desc[i];
            continue;
        }
        unsigned n = b & 3 ? 1u << ((b & 3) - 1) : 0;  // 0, 1, 2 or 4 bytes
        if (i + n > len)
            break;
        uint32_t u = 0;
        for (unsigned k = 0; k < n; ++k)
            u |= (uint32_t)desc[i + k] << (8 * k);
        int32_t s = n == 1 ? (int8_t)u : n == 2 ? (int16_t)u : (int32_t)u;
        i += n;
        unsigned type = (b >> 2) & 3, tag = b >> 4;

        if (type == 1) {  // global
            switch (tag) {
            case 0x0: g.page = (uint16_t)u; break;
            case 0x1: g.min = s; break;
            case 0x2: g.max = s; g.max_raw = u; break;
            case 0x7: g.size = u; break;
            case 0x8: g.id = u; have_id = true; break;
            case 0x9: g.count = u; break;
            case 0xa: if (depth < 4) stack[depth++] = g; break;
            case 0xb: if (depth > 0) g = stack[--depth]; break;
            }
            continue;
        }
        if (type == 2) {  // local
            uint32_t full = n == 4 ? u : (uint32_t)g.page << 16 | u;
            if (tag == 0x0 && nusages < MAX_USAGES)
                usages[nusages++] = full;
            else if (tag == 0x1)
                umin = full, range = true;
            else if (tag == 0x2)
                umax = full, range = true;
            continue;
        }
        if (type == 0 && tag == 0x8) {  // Input
            uint16_t *pos = bits_for(&rb, (uint8_t)g.id);
            if (!pos)
                break;
            // A maximum like 255 written in one byte reads as -1: it was
            // meant unsigned.
            globals fg = g;
            if (fg.max < fg.min)
                fg.max = (int32_t)g.max_raw;
            bool constant = u & 1, variable = u & 2, relative = u & 4;
            for (uint32_t k = 0; k < g.count; ++k) {
                uint32_t bit = *pos + k * g.size;
                uint32_t usage = range ? umin + k : k < (uint32_t)nusages ? usages[k]
                                                  : nusages ? usages[nusages - 1] : 0;
                if (range && umin + k > umax)
                    usage = 0;
                if (constant || !variable)
                    continue;
                if (lay.x.valid && (uint8_t)g.id != lay.report_id)
                    continue;  // only the report with the stick
                uint16_t page = (uint16_t)(usage >> 16), id = (uint16_t)usage;
                if (page == PAGE_DESKTOP && !relative) {
                    hid_field *f = id == USAGE_X ? &lay.x : id == USAGE_Y ? &lay.y
                                 : id == USAGE_HAT ? &lay.hat : NULL;
                    if (f && !f->valid) {
                        set_field(f, bit, &fg);
                        lay.report_id = (uint8_t)g.id;
                    }
                } else if (page == PAGE_BUTTON && fg.size == 1 && id >= 1 && id <= 32 &&
                           bit >= id - 1u) {
                    // The first run of buttons: button n at button_bit + n - 1.
                    if (!lay.buttons) {
                        lay.button_bit = (uint16_t)(bit - (id - 1));
                        button_id = (uint8_t)g.id;
                    }
                    if ((uint8_t)g.id == button_id && bit - (id - 1) == lay.button_bit &&
                        id > lay.buttons)
                        lay.buttons = (uint8_t)id;
                }
            }
            *pos = (uint16_t)(*pos + g.count * g.size);
        }
        if (type == 0) {  // any main item ends the local items
            nusages = 0;
            range = false;
            umin = umax = 0;
        }
    }
    if (!have_id)
        lay.report_id = 0;
    if (button_id != lay.report_id)
        lay.buttons = 0;  // in another report: not read
    bool stick = lay.x.valid && lay.y.valid;
    if (!stick && !lay.hat.valid)
        return false;
    *out = lay;
    return true;
}

/// A field's raw value from a report.
/// @param f the field
/// @param data the report
/// @param len its length
/// @return the value
static int32_t field_value(const hid_field *f, const uint8_t *data, size_t len) {
    uint32_t v = 0;
    for (unsigned k = 0; k < f->size && k < 32; ++k) {
        unsigned bit = f->bit + k;
        if (bit / 8 < len && (data[bit / 8] >> (bit % 8) & 1))
            v |= 1u << k;
    }
    if (f->is_signed && f->size < 32 && (v >> (f->size - 1) & 1))
        v |= ~0u << f->size;
    return (int32_t)v;
}

/// A field's value scaled to 0-255 (127 if it isn't there).
/// @param f the field
/// @param data the report
/// @param len its length
/// @return 0-255
static uint8_t scale(const hid_field *f, const uint8_t *data, size_t len) {
    if (!f->valid || f->max <= f->min)
        return 127;
    int32_t v = field_value(f, data, len);
    if (v < f->min)
        v = f->min;
    if (v > f->max)
        v = f->max;
    return (uint8_t)((int64_t)(v - f->min) * 255 / (f->max - f->min));
}

bool hid_gamepad_decode(const hid_gamepad_layout *g, const uint8_t *report, size_t len,
                        uint8_t *x, uint8_t *y, uint32_t *buttons) {
    if (g->report_id) {
        if (!len || report[0] != g->report_id)
            return false;
        ++report;
        --len;
    }
    *x = scale(&g->x, report, len);
    *y = scale(&g->y, report, len);
    if (g->hat.valid) {
        // 0 up, then clockwise in 45 degree steps; out of range is centred.
        static const int8_t dx[8] = {0, 1, 1, 1, 0, -1, -1, -1};
        static const int8_t dy[8] = {-1, -1, 0, 1, 1, 1, 0, -1};
        int32_t h = field_value(&g->hat, report, len) - g->hat.min;
        if (h >= 0 && h < 8 && g->hat.max - g->hat.min >= 7) {
            *x = (uint8_t)(127 + dx[h] * 127 + (dx[h] > 0));
            *y = (uint8_t)(127 + dy[h] * 127 + (dy[h] > 0));
        }
    }
    uint32_t b = 0;
    for (unsigned i = 0; i < g->buttons; ++i) {
        unsigned bit = g->button_bit + i;
        if (bit / 8 < len && (report[bit / 8] >> (bit % 8) & 1))
            b |= 1u << i;
    }
    *buttons = b;
    return true;
}
