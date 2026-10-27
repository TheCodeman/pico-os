/**
 * @file usb_keyboard.c
 * @brief Implementation of usb_keyboard.h: uSB keyboards on the native USB port (directly or through a hub), turned into characters.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */

#include "usb_keyboard.h"

#include "hid_gamepad.h"
#include "mouse.h"
#include "radio.h"
#include "screenshot.h"
#include "hardware/sync.h"
#include "hardware/watchdog.h"
#include "pico/time.h"
#include "pico/util/queue.h"
#include "tusb.h"

/// How long a key is held before it repeats.
#define REPEAT_DELAY_MS 500
#define REPEAT_RATE_MS  33  ///< about 30 characters a second

/// Either shift.
#define SHIFT (KEYBOARD_MODIFIER_LEFTSHIFT | KEYBOARD_MODIFIER_RIGHTSHIFT)
/// Either control.
#define CTRL  (KEYBOARD_MODIFIER_LEFTCTRL | KEYBOARD_MODIFIER_RIGHTCTRL)
/// Either alt.
#define ALT   (KEYBOARD_MODIFIER_LEFTALT | KEYBOARD_MODIFIER_RIGHTALT)

static queue_t chars;  ///< multicore-safe: core 1 adds, core 0 removes

static struct {
    /// Its USB address and HID instance.
    ///@{
    uint8_t dev_addr, instance;
    ///@}
} keyboards[CFG_TUH_HID];  ///< the keyboards
/// Their count.
static volatile int keyboard_count;

/// Caps Lock is on.
static bool caps_lock;
static uint8_t leds;  ///< static: the LED report is sent asynchronously
/// LED reports go out from usb_keyboard_task(), never from a TinyUSB callback
/// (mount callbacks run in the middle of enumeration), and only on the
/// keyboard's interrupt OUT endpoint, never as a control transfer: the RP2350
/// host does control and bulk transfers on one shared endpoint, and TinyUSB
/// panics if a control transfer starts while a flash drive is mid-transfer.
/// A keyboard with no OUT endpoint just doesn't get its Caps Lock LED.
static bool leds_pending;
/// When the LED report may next go.
static absolute_time_t leds_at;
/// The last report.
static hid_keyboard_report_t prev_report;
static volatile uint8_t held_mods;  ///< for core 0: the last report's modifiers
static volatile bool held_key;      ///< and whether it had a key
/// The last report's keys for core 0, written between two changes of
/// held_seq (odd while being written) so a reader can tell it got a whole one.
static volatile uint32_t held_seq;
/// The keys and modifiers.
///@{
static volatile uint8_t held_keys[6], held_keys_mods;
///@}

/// Two gamepads: the non-keyboard HID interfaces with a stick, first come
/// first in slot 0; a slot keeps its pad until it is unplugged.
#define PADS 2
static struct {
    bool mounted;  ///< the slot is in use
    ///@{
    uint8_t dev_addr, instance;
    ///@}
    hid_gamepad_layout layout;  ///< where its values are
} pad[PADS];  ///< the pads
static volatile uint32_t pad_state[PADS];  ///< x | y << 8 | buttons << 16, for core 0
/// Each pad is plugged in.
static volatile bool pad_present[PADS];

/// A pad's slot.
/// @param dev_addr its address
/// @param instance its HID instance
/// @return the slot, or -1
static int pad_slot(uint8_t dev_addr, uint8_t instance) {
    for (int i = 0; i < PADS; ++i)
        if (pad[i].mounted && pad[i].dev_addr == dev_addr && pad[i].instance == instance)
            return i;
    return -1;
}
/// The key repeating and its modifiers.
///@{
static uint8_t repeat_key, repeat_mods;
///@}
/// When it next repeats.
static absolute_time_t repeat_at;

/// US layout, {unshifted, shifted}, indexed by HID usage code.
static const uint8_t keycode_to_ascii[128][2] = {HID_KEYCODE_TO_ASCII};

/// Queues a string (an escape sequence).
/// @param s it
static void put_string(const char *s) {
    for (; *s; ++s)
        queue_try_add(&chars, s);
}

/// Queues what a key types: a character or an escape sequence.
/// @param key the HID usage
/// @param mods the modifiers
static void put_key(uint8_t key, uint8_t mods) {
    if (key == HID_KEY_PRINT_SCREEN || (key == HID_KEY_S && (mods & CTRL) && (mods & ALT))) {
        screenshot_request();
        return;
    }
    // Ctrl+Alt+M (or a keyboard's Mute key): the radio's mute, from anywhere.
    if (key == HID_KEY_MUTE || (key == HID_KEY_M && (mods & CTRL) && (mods & ALT))) {
        radio_hotkey_mute();
        return;
    }
    switch (key) {
    case HID_KEY_ARROW_UP:    put_string("\x1b[A"); return;
    case HID_KEY_ARROW_DOWN:  put_string("\x1b[B"); return;
    case HID_KEY_ARROW_RIGHT: put_string("\x1b[C"); return;
    case HID_KEY_ARROW_LEFT:  put_string("\x1b[D"); return;
    case HID_KEY_HOME:        put_string("\x1b[H"); return;
    case HID_KEY_END:         put_string("\x1b[F"); return;
    case HID_KEY_INSERT:      put_string("\x1b[2~"); return;
    case HID_KEY_DELETE:      put_string("\x1b[3~"); return;
    case HID_KEY_PAGE_UP:     put_string("\x1b[5~"); return;
    case HID_KEY_PAGE_DOWN:   put_string("\x1b[6~"); return;
    // Function keys as xterm sends them (F3 is find next in the editor;
    // BASIC uses them as soft keys and for ON KEY).
    case HID_KEY_F1:          put_string("\x1bOP"); return;
    case HID_KEY_F2:          put_string("\x1bOQ"); return;
    case HID_KEY_F3:          put_string("\x1bOR"); return;
    case HID_KEY_F4:          put_string("\x1bOS"); return;
    case HID_KEY_F5:          put_string("\x1b[15~"); return;
    case HID_KEY_F6:          put_string("\x1b[17~"); return;
    case HID_KEY_F7:          put_string("\x1b[18~"); return;
    case HID_KEY_F8:          put_string("\x1b[19~"); return;
    case HID_KEY_F9:          put_string("\x1b[20~"); return;
    case HID_KEY_F10:         put_string("\x1b[21~"); return;
    case HID_KEY_F11:         put_string("\x1b[23~"); return;
    case HID_KEY_F12:         put_string("\x1b[24~"); return;
    }
    if (key >= 128)
        return;
    bool letter = key >= HID_KEY_A && key <= HID_KEY_Z;
    bool shifted = (mods & SHIFT) != 0;
    if (letter && caps_lock)
        shifted = !shifted;
    char c = keycode_to_ascii[key][shifted];
    if (!c)
        return;
    if ((mods & CTRL) && ((c >= 'a' && c <= 'z') || (c >= '@' && c <= '_')))
        c &= 0x1f;  // Ctrl-A is 1, Ctrl-G (bell) is 7, and so on
    if (mods & ALT) {
        char esc = 0x1b;  // Alt-x as Esc x, as xterm sends it (mc, ssh)
        queue_try_add(&chars, &esc);
    }
    queue_try_add(&chars, &c);
}

/// Asks for the LED report to be sent.
static void update_leds(void) {
    leds_pending = true;
}

/// One keyboard per call. A keyboard whose endpoint is busy is tried again a
/// few times, then skipped.
static void send_leds(void) {
    static int next, tries;
    if (!leds_pending || !time_reached(leds_at))
        return;
    if (next >= keyboard_count) {
        next = 0;
        leds_pending = false;
        return;
    }
    leds = caps_lock ? KEYBOARD_LED_CAPSLOCK : 0;
    if (tuh_hid_send_report(keyboards[next].dev_addr, keyboards[next].instance, 0, &leds,
                            sizeof leds) ||
        ++tries >= 10) {
        ++next;
        tries = 0;
    }
    leds_at = make_timeout_time_ms(20);
}

/// Whether a report has a key.
/// @param r the report
/// @param key the HID usage
/// @return it has
static bool report_has_key(const hid_keyboard_report_t *r, uint8_t key) {
    for (int i = 0; i < 6; ++i)
        if (r->keycode[i] == key)
            return true;
    return false;
}

/// Publishes the held keys for core 0.
/// @param r the report, or NULL for none
static void set_held_keys(const hid_keyboard_report_t *r) {
    held_seq = held_seq + 1;
    __dmb();
    for (int i = 0; i < 6; ++i)
        held_keys[i] = r ? r->keycode[i] : 0;
    held_keys_mods = r ? r->modifier : 0;
    __dmb();
    held_seq = held_seq + 1;
}

/// Boot-protocol reports list up to six keys held down. A key is newly
/// pressed if it wasn't in the previous report.
/// @param r the report
static void handle_report(const hid_keyboard_report_t *r) {
    if (r->keycode[0] == 0x01)
        return;  // rollover error: too many keys held, report is meaningless
    for (int i = 0; i < 6; ++i) {
        uint8_t key = r->keycode[i];
        if (key == HID_KEY_NONE || report_has_key(&prev_report, key))
            continue;
        // Ctrl-Alt-Del restarts at once, like a reset button (this is core 1,
        // so nothing gets tidied up first; the shell's reset command does).
        if (key == HID_KEY_DELETE && (r->modifier & CTRL) && (r->modifier & ALT))
            watchdog_reboot(0, 0, 0);
        if (key == HID_KEY_CAPS_LOCK) {
            caps_lock = !caps_lock;
            update_leds();
            continue;
        }
        put_key(key, r->modifier);
        repeat_key = key;
        repeat_at = make_timeout_time_ms(REPEAT_DELAY_MS);
    }
    if (repeat_key && !report_has_key(r, repeat_key))
        repeat_key = 0;
    repeat_mods = r->modifier;
    prev_report = *r;
    held_mods = r->modifier;
    held_key = r->keycode[0] != HID_KEY_NONE;
    set_held_keys(r);
}

/// TinyUSB: a HID interface mounted (keyboard, mouse or gamepad).
/// @param dev_addr its address
/// @param instance its HID instance
/// @param desc_report its report descriptor
/// @param desc_len its length
void tuh_hid_mount_cb(uint8_t dev_addr, uint8_t instance, uint8_t const *desc_report,
                      uint16_t desc_len) {
    if (tuh_hid_interface_protocol(dev_addr, instance) != HID_ITF_PROTOCOL_KEYBOARD) {
        if (mouse_mounted(dev_addr, instance,
                          tuh_hid_interface_protocol(dev_addr, instance) == HID_ITF_PROTOCOL_MOUSE,
                          desc_report, desc_len)) {
            tuh_hid_receive_report(dev_addr, instance);
            return;
        }
        for (int i = 0; i < PADS; ++i) {
            if (pad[i].mounted)
                continue;
            if (hid_gamepad_parse(desc_report, desc_len, &pad[i].layout)) {
                pad[i].mounted = true;
                pad[i].dev_addr = dev_addr;
                pad[i].instance = instance;
                pad_state[i] = 127 | 127 << 8;
                pad_present[i] = true;
                tuh_hid_receive_report(dev_addr, instance);
            }
            break;
        }
        return;
    }
    if (keyboard_count < CFG_TUH_HID) {
        keyboards[keyboard_count].dev_addr = dev_addr;
        keyboards[keyboard_count].instance = instance;
        keyboard_count = keyboard_count + 1;
    }
    // Give enumeration time to finish the device's other interfaces first.
    leds_at = make_timeout_time_ms(500);
    update_leds();
    tuh_hid_receive_report(dev_addr, instance);
}

/// TinyUSB: a HID interface gone.
/// @param dev_addr its address
/// @param instance its HID instance
void tuh_hid_umount_cb(uint8_t dev_addr, uint8_t instance) {
    mouse_unmounted(dev_addr, instance);
    int p = pad_slot(dev_addr, instance);
    if (p >= 0) {
        pad[p].mounted = false;
        pad_present[p] = false;
        return;
    }
    for (int i = 0; i < keyboard_count; ++i) {
        if (keyboards[i].dev_addr == dev_addr && keyboards[i].instance == instance) {
            keyboards[i] = keyboards[keyboard_count - 1];
            keyboard_count = keyboard_count - 1;
            break;
        }
    }
    repeat_key = 0;
    prev_report = (hid_keyboard_report_t){0};
    held_mods = 0;
    held_key = false;
    set_held_keys(NULL);
}

/// TinyUSB: a HID report.
/// @param dev_addr its address
/// @param instance its HID instance
/// @param report the report
/// @param len its length
void tuh_hid_report_received_cb(uint8_t dev_addr, uint8_t instance, uint8_t const *report,
                                uint16_t len) {
    if (tuh_hid_interface_protocol(dev_addr, instance) == HID_ITF_PROTOCOL_KEYBOARD &&
        len >= sizeof(hid_keyboard_report_t))
        handle_report((const hid_keyboard_report_t *)report);
    else if (!mouse_report(dev_addr, instance, report, len)) {
        int p = pad_slot(dev_addr, instance);
        uint8_t x, y;
        uint32_t buttons;
        if (p >= 0 && hid_gamepad_decode(&pad[p].layout, report, len, &x, &y, &buttons))
            pad_state[p] = x | (uint32_t)y << 8 | (buttons & 0xffff) << 16;
    }
    tuh_hid_receive_report(dev_addr, instance);
}

void usb_keyboard_init(void) {
    queue_init(&chars, 1, 64);
}

void usb_keyboard_start(void) {
    tuh_init(0);
}

void usb_keyboard_task(void) {
    tuh_task();
    send_leds();
    if (repeat_key && time_reached(repeat_at)) {
        put_key(repeat_key, repeat_mods);
        repeat_at = make_timeout_time_ms(REPEAT_RATE_MS);
    }
}

int usb_keyboard_count(void) {
    return keyboard_count;
}

uint8_t usb_keyboard_modifiers(void) {
    return held_mods;
}

bool usb_keyboard_key_held(void) {
    return held_key;
}

bool usb_keyboard_held_keys(uint8_t keys[6], uint8_t *mods) {
    uint32_t seq;
    do {
        while ((seq = held_seq) & 1) {
        }
        __dmb();
        for (int i = 0; i < 6; ++i)
            keys[i] = held_keys[i];
        *mods = held_keys_mods;
        __dmb();
    } while (held_seq != seq);
    return keyboard_count > 0;
}

bool usb_keyboard_caps_lock(void) {
    return caps_lock;
}

void usb_keyboard_set_caps_lock(bool on) {
    caps_lock = on;
    update_leds();
}

bool usb_gamepad_read(uint8_t *x, uint8_t *y, uint32_t *buttons) {
    return usb_gamepad_read_n(0, x, y, buttons);
}

bool usb_gamepad_read_n(unsigned n, uint8_t *x, uint8_t *y, uint32_t *buttons) {
    if (n >= PADS || !pad_present[n])
        return false;
    uint32_t s = pad_state[n];
    *x = (uint8_t)s;
    *y = (uint8_t)(s >> 8);
    *buttons = s >> 16;
    return true;
}

/// A typed character.
/// @return it, or -1
static int raw_getc(void) {
    char c;
    return queue_try_remove(&chars, &c) ? (uint8_t)c : -1;
}

/// The SDK's stdio input: the typed characters.
/// @param buf where
/// @param len room
/// @return the count, or PICO_ERROR_NO_DATA
static int stdio_in_chars(char *buf, int len) {
    screenshot_poll(raw_getc);  // Print Screen was pressed: the box takes the keys
    radio_note_poll();          // the "Radio muted" note: shown, then taken away
    // Nothing typed: answered without the queue's spin lock. Programs poll
    // this in tight loops (BASIC every statement), and taking the lock each
    // time kept core 1's audio interrupt waiting on a lock it shares.
    if (queue_get_level_unsafe(&chars) == 0)
        return PICO_ERROR_NO_DATA;
    int n = 0;
    while (n < len && queue_try_remove(&chars, &buf[n]))
        ++n;
    return n ? n : PICO_ERROR_NO_DATA;
}

stdio_driver_t usb_keyboard_stdio_driver = {
    .in_chars = stdio_in_chars,
};
