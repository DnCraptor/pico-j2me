/*
 * pal_rp2350.c - the platform interface (src/pal/pal.h) on the RP2350:
 * time, the console log, phone keys, the screen and the data directory.
 *
 * Keyboard and gamepad map to phone keys (MIDP key codes):
 *
 *   keyboard                     gamepad          phone
 *   arrows                       D-pad            Up / Down / Left / Right
 *   Enter, Space                 A                Fire
 *   F1                           Start            left soft key
 *   F2, Esc                      B                right soft key
 *   0..9 (and the keypad)        X = 5, Y = 0     0..9
 *   Shift+8, keypad *            L                *
 *   Shift+3, keypad /            R                #
 *   Backspace, Delete                             Clear (-8)
 *   F12                          Select           the emulator menu
 */
#include "rp2350.h"
#include "../../src/vm/vm.h"

#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "hardware/sync.h"
#include "tusb.h"   /* HID_KEY_* */

#ifndef NJ_BUILD_EPOCH
#define NJ_BUILD_EPOCH 1767225600LL   /* 2026-01-01 */
#endif

int nj_exit_action;

/* ------------------------------------------------------------------------ */
/* Console log: kept in RAM for the error screen, also sent to stdio.       */

#define LOG_SIZE 4096

static char log_buf[LOG_SIZE];
static int  log_len;

void pal_console_write(const char *s, int len)
{
    for (int i = 0; i < len; i++) {
        if (log_len == LOG_SIZE) {
            memmove(log_buf, log_buf + LOG_SIZE / 2, LOG_SIZE / 2);
            log_len = LOG_SIZE / 2;
        }
        log_buf[log_len++] = s[i];
    }
    fwrite(s, 1, (size_t)len, stdout);
}

void pal_log_tail(char *out, size_t size, int lines)
{
    int start = log_len;
    while (start > 0 && lines > 0) {
        start--;
        if (log_buf[start] == '\n' && start != log_len - 1)
            lines--;
    }
    if (start > 0)
        start++;
    size_t n = (size_t)(log_len - start);
    if (n >= size)
        n = size - 1;
    memcpy(out, log_buf + log_len - n, n);
    out[n] = 0;
}

void pal_fatal(const char *msg)
{
    audio_pause(true);
    ui_message("pico-j2me stopped", msg, true);
    nj_safe_reset();
}

/* ------------------------------------------------------------------------ */
/* Time                                                                     */

static int64_t paused_us;   /* time spent in the emulator menu */

int64_t pal_time_ms(void)
{
    return ((int64_t)time_us_64() - paused_us) / 1000;
}

int64_t pal_epoch_ms(void)
{
    /* No RTC: the clock starts at the build time. */
    return NJ_BUILD_EPOCH * 1000 + (int64_t)(time_us_64() / 1000);
}

/* ------------------------------------------------------------------------ */
/* Session                                                                  */

static GameSettings *settings;
static const char   *game_jar;
static int           data_dir_state;
static int           canvas_w = 240, canvas_h = 320;

/* How many physical keys hold each phone key down: a key reaches the game
 * as down on the first press and as up when the last one is released. */
#define CODE_MIN (-16)
#define CODE_MAX 127
static uint8_t held[CODE_MAX - CODE_MIN + 1];

#define QUEUE 32
static NjEvent queue[QUEUE];
static int     q_head, q_tail;

static void push(int type, int a, int b)
{
    int next = (q_tail + 1) % QUEUE;
    if (next == q_head)
        return;
    queue[q_tail] = (NjEvent){type, a, b};
    q_tail = next;
}

void pal_session_start(GameSettings *s, const char *jar)
{
    settings = s;
    game_jar = jar;
    canvas_w = s->w;
    canvas_h = s->h;
    nj_exit_action = EXIT_NONE;
    data_dir_state = 0;
    memset(held, 0, sizeof held);
    q_head = q_tail = 0;
    input_flush();
    audio_reset();
    audio_master(s->volume);
    video_game_mode(canvas_w, canvas_h, s->view_w, s->view_h);
    audio_pause(false);
}

/* ------------------------------------------------------------------------ */
/* Input                                                                    */

#define KEY_MENU 0x1000

static int key_code(uint8_t hid)
{
    bool shift = input_shift_held();
    switch (hid) {
    case HID_KEY_ARROW_UP:     return -1;
    case HID_KEY_ARROW_DOWN:   return -2;
    case HID_KEY_ARROW_LEFT:   return -3;
    case HID_KEY_ARROW_RIGHT:  return -4;
    case HID_KEY_ENTER:        case HID_KEY_KEYPAD_ENTER: case HID_KEY_SPACE: return -5;
    case HID_KEY_F1:           return -6;
    case HID_KEY_F2:           case HID_KEY_ESCAPE: return -7;
    case HID_KEY_BACKSPACE:    case HID_KEY_DELETE: return -8;
    case HID_KEY_F12:          return KEY_MENU;
    case HID_KEY_KEYPAD_MULTIPLY: return '*';
    case HID_KEY_KEYPAD_DIVIDE:   return '#';
    case HID_KEY_3:            return shift ? '#' : '3';
    case HID_KEY_8:            return shift ? '*' : '8';
    case HID_KEY_0:            case HID_KEY_KEYPAD_0: return '0';
    default:
        break;
    }
    if (hid >= HID_KEY_1 && hid <= HID_KEY_9)
        return '1' + (hid - HID_KEY_1);
    if (hid >= HID_KEY_KEYPAD_1 && hid <= HID_KEY_KEYPAD_9)
        return '1' + (hid - HID_KEY_KEYPAD_1);
    return 0;
}

static const int pad_codes[PAD_COUNT] = {
    -5,         /* A: fire */
    -7,         /* B: right soft key (usually "back") */
    KEY_MENU,   /* Select: emulator menu */
    -6,         /* Start: left soft key (usually "menu"/"ok") */
    -1, -2, -3, -4,
    '5',        /* X */
    '0',        /* Y */
    '*',        /* L */
    '#',        /* R */
};

/* The physical key a phone key went down with: a key released after Shift
 * was let go must release the same phone key ('*' and not '8'). */
static int kbd_down_code[256];

static void phone_key(int code, bool down)
{
    if (code < CODE_MIN || code > CODE_MAX)
        return;
    uint8_t *h = &held[code - CODE_MIN];
    if (down) {
        if ((*h)++ == 0)
            push(EV_KEY_DOWN, code, 0);
    } else if (*h > 0) {
        if (--*h == 0)
            push(EV_KEY_UP, code, 0);
    }
}

static void release_all(void)
{
    for (int i = 0; i < (int)sizeof held; i++)
        if (held[i]) {
            held[i] = 0;
            push(EV_KEY_UP, i + CODE_MIN, 0);
        }
    memset(kbd_down_code, 0, sizeof kbd_down_code);
}

static void open_menu(void)
{
    uint64_t start = time_us_64();
    audio_pause(true);
    int action = ui_game_menu(settings, game_jar);
    paused_us += (int64_t)(time_us_64() - start);
    release_all();
    input_flush();
    if (action != EXIT_NONE) {
        nj_exit_action = action;
        vm.exit_requested = true;
        return;
    }
    audio_master(settings->volume);
    video_game_resume();
    audio_pause(false);
}

static void poll_input(void)
{
    RawInput in;
    while (input_get(&in)) {
        int code;
        if (in.kind == RAW_PAD) {
            code = pad_codes[in.code];
        } else if (in.down) {
            code = key_code(in.code);
            kbd_down_code[in.code] = code;
        } else {
            code = kbd_down_code[in.code];
            kbd_down_code[in.code] = 0;
        }
        if (code == KEY_MENU) {
            if (in.down) {
                open_menu();
                return;
            }
            continue;
        }
        if (code)
            phone_key(code, in.down);
    }
}

bool pal_poll_event(NjEvent *ev)
{
    if (q_head == q_tail)
        poll_input();
    if (q_head == q_tail)
        return false;
    *ev = queue[q_head];
    q_head = (q_head + 1) % QUEUE;
    return true;
}

void pal_wait(int64_t until_ms)
{
    /* Sleep until the deadline or until core 1 queues input (it sends an
     * event, __sev, with every key), at most 20 ms at a time. */
    int64_t now_us = (int64_t)time_us_64() - paused_us;
    int64_t limit_us = now_us + 20000;
    if (until_ms >= 0 && until_ms * 1000 < limit_us)
        limit_us = until_ms * 1000;
    if (limit_us <= now_us)
        return;
    absolute_time_t until = make_timeout_time_us((uint64_t)(limit_us - now_us));
    while (!input_pending() && !time_reached(until))
        best_effort_wfe_or_timeout(until);
}

/* ------------------------------------------------------------------------ */
/* Screen                                                                   */

void pal_screen_size(int *w, int *h)
{
    *w = canvas_w;
    *h = canvas_h;
}

void pal_present(const uint16_t *px, int w, int h)
{
    video_present(px, w, h);
}

void pal_soft_labels(const char *left, const char *right)
{
    (void)left;
    (void)right;
}

/* ------------------------------------------------------------------------ */
/* Storage                                                                  */

/* /.config/pico-j2me/rms/<game>, created on first use */
static char data_dir[192];   /* data_dir_state: 0 = not made yet, 1 = ok, -1 = failed */

const char *pal_data_dir(void)
{
    if (data_dir_state == 0) {
        data_dir_state = -1;
        if (nj_sd_ok && game_jar) {
            char name[128];
            settings_game_name(game_jar, name, sizeof name);
            snprintf(data_dir, sizeof data_dir, NJ_CONFIG_DIR "/rms/%s", name);
            if (sys_mkdirs(data_dir))
                data_dir_state = 1;
        }
    }
    return data_dir_state > 0 ? data_dir : NULL;
}
