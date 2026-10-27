/**
 * @file mouse.c
 * @brief USB mice: see mouse.h.
 *
 * The USB callbacks (core 1) move the pointer and queue events; core 0 takes
 * them and draws the pointer.
 *
 * A mouse interface starts in the boot protocol (TinyUSB's default) and
 * stays in it: buttons, X and Y, and the wheel in the fourth byte if its
 * report descriptor says it has one. Other
 * HID interfaces (a receiver with a keyboard and a mouse on one) are used if
 * their descriptor has a mouse in it.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */

#include "mouse.h"

#include <string.h>

#include "console.h"
#include "dvi.h"
#include "hardware/sync.h"
#include "pico/stdlib.h"
#include "tusb.h"

/// Mice at once.
#define MICE 4
#define QUEUE 64  ///< a power of two

// ---- Where things are in a report -------------------------------------------------------------------

/// Where a value is in a report.
typedef struct {
    /// Its first bit and size in bits.
    ///@{
    uint16_t bit, size;
    ///@}
    /// Signed; found.
    ///@{
    bool is_signed, valid;
    ///@}
} field_t;

/// Where a mouse's values are in its reports.
typedef struct {
    uint8_t report_id;    ///< 0: reports aren't numbered
    /// relative
    ///@{
    field_t x, y, wheel;
    ///@}
    uint16_t button_bit;  ///< the first button's bit
    uint8_t buttons;  ///< how many buttons
} layout_t;

static struct {
    /// Mounted; in report protocol (not boot).
    ///@{
    bool mounted, report_protocol;
    ///@}
    /// Its USB address and HID instance.
    ///@{
    uint8_t dev_addr, instance;
    ///@}
    layout_t layout;  ///< where its values are
} mice[MICE];  ///< the mice

/// Finds a mouse's relative X, Y, wheel and buttons in a report descriptor
/// (HID 1.11, 6.2.2): the first report with relative X and Y.
/// @param desc the descriptor
/// @param len its length
/// @param out the layout
/// @return found
static bool parse(const uint8_t *desc, size_t len, layout_t *out) {
    typedef struct {
        uint16_t page;
        int32_t min;
        ///@{
        uint32_t size, count, id;
        ///@}
    } globals;
    layout_t lay;
    memset(&lay, 0, sizeof lay);
    globals g = {0}, stack[4];
    int depth = 0;
    uint32_t usages[16];
    int nusages = 0;
    uint32_t umin = 0, umax = 0;
    bool range = false;
    // Bits used so far in each numbered report.
    uint8_t ids[16];
    uint16_t bits[16];
    int nids = 0;
    int mouse_id = -1;
    for (size_t i = 0; i < len;) {
        uint8_t b = desc[i++];
        if (b == 0xfe) {
            if (i + 1 >= len)
                break;
            i += 2 + desc[i];
            continue;
        }
        unsigned n = b & 3 ? 1u << ((b & 3) - 1) : 0;
        if (i + n > len)
            break;
        uint32_t u = 0;
        for (unsigned k = 0; k < n; ++k)
            u |= (uint32_t)desc[i + k] << (8 * k);
        int32_t s = n == 1 ? (int8_t)u : n == 2 ? (int16_t)u : (int32_t)u;
        i += n;
        unsigned type = (b >> 2) & 3, tag = b >> 4;
        if (type == 1) {
            switch (tag) {
            case 0x0: g.page = (uint16_t)u; break;
            case 0x1: g.min = s; break;
            case 0x7: g.size = u; break;
            case 0x8: g.id = u; break;
            case 0x9: g.count = u; break;
            case 0xa: if (depth < 4) stack[depth++] = g; break;
            case 0xb: if (depth > 0) g = stack[--depth]; break;
            }
            continue;
        }
        if (type == 2) {
            uint32_t full = n == 4 ? u : (uint32_t)g.page << 16 | u;
            if (tag == 0x0 && nusages < 16)
                usages[nusages++] = full;
            else if (tag == 0x1)
                umin = full, range = true;
            else if (tag == 0x2)
                umax = full, range = true;
            continue;
        }
        if (type == 0 && tag == 0x8) {  // Input
            int r = 0;
            while (r < nids && ids[r] != (uint8_t)g.id)
                ++r;
            if (r == nids) {
                if (nids == 16)
                    break;
                ids[nids] = (uint8_t)g.id;
                bits[nids++] = 0;
            }
            bool constant = u & 1, variable = u & 2, relative = u & 4;
            for (uint32_t k = 0; k < g.count && !constant && variable; ++k) {
                uint32_t bit = bits[r] + k * g.size;
                uint32_t usage = range ? umin + k : k < (uint32_t)nusages ? usages[k]
                                                  : nusages ? usages[nusages - 1] : 0;
                if (range && umin + k > umax)
                    usage = 0;
                uint16_t page = usage >> 16, id = (uint16_t)usage;
                if (mouse_id >= 0 && (uint8_t)g.id != mouse_id)
                    continue;  // only the mouse's report
                field_t f = {(uint16_t)bit, (uint16_t)g.size, g.min < 0, true};
                if (page == 0x01 && relative && (id == 0x30 || id == 0x31 || id == 0x38)) {
                    field_t *to = id == 0x30 ? &lay.x : id == 0x31 ? &lay.y : &lay.wheel;
                    if (!to->valid)
                        *to = f;
                    if (id != 0x38)
                        mouse_id = (uint8_t)g.id;
                } else if (page == 0x09 && g.size == 1 && id >= 1 && id <= 8 && bit >= id - 1u) {
                    if (!lay.buttons)
                        lay.button_bit = (uint16_t)(bit - (id - 1));
                    if (bit - (id - 1) == lay.button_bit && id > lay.buttons)
                        lay.buttons = (uint8_t)id;
                }
            }
            bits[r] = (uint16_t)(bits[r] + g.count * g.size);
        }
        if (type == 0) {
            nusages = 0;
            range = false;
            umin = umax = 0;
        }
    }
    if (!lay.x.valid || !lay.y.valid)
        return false;
    lay.report_id = mouse_id > 0 ? (uint8_t)mouse_id : 0;
    *out = lay;
    return true;
}

/// A field's value from a report.
/// @param f the field
/// @param data the report
/// @param len its length
/// @return the value
static int32_t value(const field_t *f, const uint8_t *data, size_t len) {
    if (!f->valid)
        return 0;
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

// ---- State, written on core 1 ----------------------------------------------------------------------------

/// The pointer's position.
///@{
static volatile int16_t pos_x = DVI_WIDTH / 2, pos_y = DVI_HEIGHT / 2;
///@}
/// The buttons held.
static volatile uint8_t held;
/// The wheel's movement so far.
static volatile int wheel_total;
/// Mouse reports so far.
static volatile int count;
/// The events for core 0.
static mouse_event_t queue[QUEUE];
/// The queue's ends. Head: core 1 writes; tail: core 0.
///@{
static volatile uint32_t q_head, q_tail;
///@}
/// When a button last went down, for double-clicks.
static uint32_t last_down_ms;
/// And where.
///@{
static int16_t last_down_x, last_down_y;
///@}
/// And which.
static uint8_t last_down_button;

/// Queues an event (dropped if the queue is full).
/// @param e the event
static void push(mouse_event_t e) {
    if (q_head - q_tail >= QUEUE)
        return;  // full: dropped (the position is still right)
    queue[q_head % QUEUE] = e;
    __dmb();
    q_head = q_head + 1;
}

/// Pointer speed: pixels per 16 mouse counts for each setting 1-10 (5,
/// the default, is half a pixel a count), and the leftover fractions so slow
/// moves still move.
static const uint8_t speeds[10] = {2, 3, 4, 6, 8, 10, 12, 14, 16, 20};
/// The speed setting (1-10).
static volatile int speed = 5;
/// Acceleration on.
static volatile bool accel = true;
/// Fractions of a dot left over, in sixteenths.
///@{
static int frac_x, frac_y;
///@}

void mouse_set_speed(int s, bool acceleration) {
    speed = s < 1 ? 1 : s > 10 ? 10 : s;
    accel = acceleration;
}

void mouse_get_speed(int *s, bool *acceleration) {
    *s = speed;
    *acceleration = accel;
}

/// A report's movement: the pointer moved, the buttons' changes queued.
/// @param dx X counts
/// @param dy Y counts
/// @param dw wheel counts
/// @param buttons the buttons held
static void moved(int dx, int dy, int dw, uint8_t buttons) {
    // With acceleration, fast sweeps go half as far again (above 12 counts a report).
    int ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy, k = speeds[speed - 1];
    int sx = dx * k * (accel && ax > 12 ? 3 : 2) / 2 + frac_x;
    int sy = dy * k * (accel && ay > 12 ? 3 : 2) / 2 + frac_y;
    int px = sx / 16, py = sy / 16;
    frac_x = sx - px * 16;
    frac_y = sy - py * 16;
    int x = pos_x + px, y = pos_y + py;
    x = x < 0 ? 0 : x >= DVI_WIDTH ? DVI_WIDTH - 1 : x;
    y = y < 0 ? 0 : y >= DVI_HEIGHT ? DVI_HEIGHT - 1 : y;
    bool move = x != pos_x || y != pos_y;
    pos_x = x;
    pos_y = y;
    if (move)
        push((mouse_event_t){MOUSE_MOVE, 0, held, 0, (int16_t)x, (int16_t)y, false});
    uint8_t was = held;
    for (int b = 0; b < 3; ++b) {
        uint8_t bit = 1 << b;
        if ((buttons & bit) == (was & bit))
            continue;
        held = (held & ~bit) | (buttons & bit);
        mouse_event_t e = {(buttons & bit) ? MOUSE_DOWN : MOUSE_UP, bit, held, 0, (int16_t)x,
                           (int16_t)y, false};
        if (e.type == MOUSE_DOWN) {
            uint32_t now = to_ms_since_boot(get_absolute_time());
            int ddx = x - last_down_x, ddy = y - last_down_y;
            e.double_click = bit == last_down_button && now - last_down_ms < 400 &&
                             ddx * ddx + ddy * ddy <= 16;
            last_down_ms = e.double_click ? 0 : now;
            last_down_x = x;
            last_down_y = y;
            last_down_button = bit;
        }
        push(e);
    }
    held = buttons & 7;
    if (dw) {
        wheel_total += dw;
        push((mouse_event_t){MOUSE_WHEEL, 0, held, (int8_t)(dw > 127 ? 127 : dw < -127 ? -127 : dw),
                             (int16_t)x, (int16_t)y, false});
    }
}

/// A mouse's slot.
/// @param dev_addr its address
/// @param instance its HID instance
/// @return the slot, or -1
static int slot(uint8_t dev_addr, uint8_t instance) {
    for (int i = 0; i < MICE; ++i)
        if (mice[i].mounted && mice[i].dev_addr == dev_addr && mice[i].instance == instance)
            return i;
    return -1;
}

bool mouse_mounted(uint8_t dev_addr, uint8_t instance, bool boot_protocol, const uint8_t *desc,
                   uint16_t desc_len) {
    layout_t lay;
    bool parsed = parse(desc, desc_len, &lay);
    if (!boot_protocol && !parsed)
        return false;  // not a mouse
    int again = slot(dev_addr, instance);
    if (again >= 0) {
        mice[again].mounted = false;  // mounted again without going: not twice
        count = count - 1;
    }
    for (int i = 0; i < MICE; ++i) {
        if (mice[i].mounted)
            continue;
        mice[i].mounted = true;
        mice[i].dev_addr = dev_addr;
        mice[i].instance = instance;
        mice[i].layout = lay;
        mice[i].report_protocol = !boot_protocol;
        // A boot-protocol mouse stays in it: switching to full reports takes
        // a control request, which this USB host can't make safely while
        // other transfers run (the mouse went quiet). Boot reports carry the
        // wheel in their fourth byte when the mouse has one.
        count = count + 1;
        return true;
    }
    return true;  // a mouse, but too many: ignored
}

void mouse_reset(void) {
    for (int i = 0; i < MICE; ++i)
        mice[i].mounted = false;
    count = 0;
    if (held)
        moved(0, 0, 0, 0);
}

void mouse_unmounted(uint8_t dev_addr, uint8_t instance) {
    int i = slot(dev_addr, instance);
    if (i < 0)
        return;
    mice[i].mounted = false;
    count = count - 1;
    if (!count && held)
        moved(0, 0, 0, 0);  // let go of the buttons
}

bool mouse_report(uint8_t dev_addr, uint8_t instance, const uint8_t *r, uint16_t len) {
    int i = slot(dev_addr, instance);
    if (i < 0)
        return false;
    if (!mice[i].report_protocol) {
        // Boot protocol: buttons, X, Y (and often the wheel next).
        if (len >= 3)
            moved((int8_t)r[1], (int8_t)r[2],
                  len >= 4 && mice[i].layout.wheel.valid ? (int8_t)r[3] : 0, r[0]);
        return true;
    }
    const layout_t *l = &mice[i].layout;
    if (l->report_id) {
        if (!len || r[0] != l->report_id)
            return true;  // another of its reports (a keyboard's, volume keys ...)
        ++r;
        --len;
    }
    uint8_t buttons = 0;
    for (int b = 0; b < l->buttons && b < 8; ++b) {
        unsigned bit = l->button_bit + b;
        if (bit / 8 < len && (r[bit / 8] >> (bit % 8) & 1))
            buttons |= 1 << b;
    }
    moved(value(&l->x, r, len), value(&l->y, r, len), value(&l->wheel, r, len), buttons);
    return true;
}

/// TinyUSB says the protocol changed (or that it couldn't).
/// @param dev_addr its address
/// @param instance its HID instance
/// @param protocol the protocol now
void tuh_hid_set_protocol_complete_cb(uint8_t dev_addr, uint8_t instance, uint8_t protocol) {
    int i = slot(dev_addr, instance);
    if (i >= 0)
        mice[i].report_protocol = protocol == HID_PROTOCOL_REPORT;
}

// ---- Core 0 ----------------------------------------------------------------------------------------------

int mouse_count(void) {
    return count;
}

void mouse_get(int *x, int *y, int *buttons) {
    *x = pos_x;
    *y = pos_y;
    *buttons = held;
}

bool mouse_event(mouse_event_t *e) {
    for (;;) {
        if (q_tail == q_head)
            return false;
        __dmb();
        *e = queue[q_tail % QUEUE];
        q_tail = q_tail + 1;
        // A move with another move after it: only the last counts.
        if (e->type == MOUSE_MOVE && q_tail != q_head && queue[q_tail % QUEUE].type == MOUSE_MOVE)
            continue;
        return true;
    }
}

void mouse_flush(void) {
    q_tail = q_head;
}

int mouse_wheel(void) {
    static int seen;
    int now = wheel_total, d = now - seen;
    seen = now;
    return d;
}

void mouse_warp(int x, int y) {
    pos_x = x < 0 ? 0 : x >= DVI_WIDTH ? DVI_WIDTH - 1 : x;
    pos_y = y < 0 ? 0 : y >= DVI_HEIGHT ? DVI_HEIGHT - 1 : y;
}

// ---- The arrow ---------------------------------------------------------------------------------------------

/// The arrow itself is drawn by the scanout over the picture (dvi.c), so
/// nothing here touches the framebuffer: no save-under to get out of step
/// with what is drawn beneath it, and no flicker from taking it off and
/// putting it back around each redraw.
static int mode;

void mouse_pointer(int m) {
    if (m == mode)
        return;
    int was = mode;
    mode = m;
    mouse_pointer_update();
    if (was == MOUSE_POINTER_TEXT)
        console_pointer_off();
}

int mouse_pointer_mode(void) {
    return mode;
}

/// Hiding and showing are no longer needed around drawing; kept so callers
/// still work. The arrow shows whenever it's on and a mouse is plugged in.
void mouse_pointer_hide(void) {
}

void mouse_pointer_show(void) {
    mouse_pointer_update();
}

void mouse_pointer_update(void) {
    bool on = mode == MOUSE_POINTER_ARROW && count;
    if (on && (dvi_arrow_x != pos_x || dvi_arrow_y != pos_y)) {
        dvi_arrow_x = pos_x;
        dvi_arrow_y = pos_y;
    }
    if (dvi_arrow_on != on)
        dvi_arrow_on = on;
}
