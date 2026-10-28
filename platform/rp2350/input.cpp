/*
 * input.cpp - keyboard (PS/2 or USB) and NES/SNES gamepad.
 *
 * Core 1 polls the devices and queues what changed (a key or pad button
 * going down or up) in a single-producer ring; core 0 reads the ring and
 * decides what it means: phone keys for the game (pal_rp2350.c) or
 * navigation in the launcher and the menus (ui_key_wait).
 *
 * Ctrl+Alt+Del reboots from core 1, whatever core 0 is doing.
 */
#include "rp2350.h"

#include <string.h>

#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hardware/sync.h"

#include "ps2kbd_mrmltr.h"
#include "nespad.h"

#ifdef KBDUSB
#include "tusb.h"
#endif

/* ------------------------------------------------------------------------ */
/* Ring: core 1 -> core 0                                                   */

#define RING 128

static volatile uint32_t ring[RING];
static volatile uint32_t ring_head;   /* written by core 0 */
static volatile uint32_t ring_tail;   /* written by core 1 */
static volatile bool     shift_held;

static void __not_in_flash_func(push)(uint8_t kind, uint8_t code, bool down)
{
    uint32_t tail = ring_tail;
    if (tail - ring_head >= RING)
        return;   /* full: drop */
    ring[tail % RING] = (uint32_t)kind << 16 | (uint32_t)code << 8 | (down ? 1u : 0u);
    __dmb();
    ring_tail = tail + 1;
    __sev();      /* wakes core 0 from pal_wait() */
}

extern "C" bool input_get(RawInput *in)
{
    uint32_t head = ring_head;
    if (head == ring_tail)
        return false;
    __dmb();
    uint32_t v = ring[head % RING];
    __dmb();
    ring_head = head + 1;
    in->kind = (uint8_t)(v >> 16);
    in->code = (uint8_t)(v >> 8);
    in->down = (v & 1) != 0;
    return true;
}

extern "C" bool input_pending(void)
{
    return ring_head != ring_tail;
}

extern "C" void input_flush(void)
{
    ring_head = ring_tail;
}

extern "C" bool input_shift_held(void)
{
    return shift_held;
}

/* ------------------------------------------------------------------------ */
/* Keyboard                                                                 */

static bool in_report(const hid_keyboard_report_t *r, uint8_t code)
{
    for (uint8_t k : r->keycode)
        if (k == code)
            return true;
    return false;
}

extern "C" void __not_in_flash_func(process_kbd_report)(hid_keyboard_report_t const *report,
                                                         hid_keyboard_report_t const *prev)
{
    /* modifiers: bit i of the modifier byte is HID usage 0xE0 + i */
    uint8_t changed = report->modifier ^ prev->modifier;
    for (int i = 0; i < 8; i++)
        if (changed & (1u << i))
            push(RAW_KEY, (uint8_t)(HID_KEY_CONTROL_LEFT + i), (report->modifier >> i) & 1);
    shift_held = (report->modifier & (KEYBOARD_MODIFIER_LEFTSHIFT | KEYBOARD_MODIFIER_RIGHTSHIFT)) != 0;

    for (uint8_t k : prev->keycode)
        if (k >= HID_KEY_A && !in_report(report, k))
            push(RAW_KEY, k, false);
    for (uint8_t k : report->keycode)
        if (k >= HID_KEY_A && !in_report(prev, k))
            push(RAW_KEY, k, true);

    const uint8_t ctrl = KEYBOARD_MODIFIER_LEFTCTRL | KEYBOARD_MODIFIER_RIGHTCTRL;
    const uint8_t alt = KEYBOARD_MODIFIER_LEFTALT | KEYBOARD_MODIFIER_RIGHTALT;
    if ((report->modifier & ctrl) && (report->modifier & alt) &&
        (in_report(report, HID_KEY_DELETE) || in_report(report, HID_KEY_KEYPAD_DECIMAL)))
        nj_safe_reset();
}

static Ps2Kbd_Mrmltr ps2kbd(pio1, PS2KBD_GPIO_FIRST,
                            [](hid_keyboard_report_t *curr, hid_keyboard_report_t *prev) {
                                process_kbd_report(curr, prev);
                            });

/* ------------------------------------------------------------------------ */
/* Gamepad                                                                  */

static const uint32_t pad_masks[PAD_COUNT] = {
    DPAD_A, DPAD_B, DPAD_SELECT, DPAD_START, DPAD_UP, DPAD_DOWN, DPAD_LEFT, DPAD_RIGHT,
    DPAD_X, DPAD_Y, DPAD_LT, DPAD_RT,
};

static uint32_t pad_prev;

static void pad_poll(void)
{
    nespad_read();
    uint32_t now = nespad_state;
    uint32_t changed = now ^ pad_prev;
    if (!changed)
        return;
    for (int b = 0; b < PAD_COUNT; b++)
        if (changed & pad_masks[b])
            push(RAW_PAD, (uint8_t)b, (now & pad_masks[b]) != 0);
    pad_prev = now;
}

/* ------------------------------------------------------------------------ */

extern "C" void input_init(void)
{
    ps2kbd.init_gpio();
    nespad_begin(clock_get_hz(clk_sys) / 1000, NES_GPIO_CLK, NES_GPIO_DATA, NES_GPIO_LAT);
}

extern "C" void input_core1_init(void)
{
#ifdef KBDUSB
    tuh_init(BOARD_TUH_RHPORT);
#endif
}

extern "C" void input_core1_poll(void)
{
    static uint64_t last;
    uint64_t now = time_us_64();
    if (now - last >= 5000) {   /* 200 Hz */
        last = now;
        ps2kbd.tick();
        pad_poll();
    }
#ifdef KBDUSB
    tuh_task();
#endif
}
