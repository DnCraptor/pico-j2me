/*
 * video.c - the screen: VGA or HDMI through the Murmulator drivers.
 *
 * The phone screen is drawn by the VM in 15-bit colour (the DS layout of
 * the class library: red in the low bits).  pal_present() turns each frame
 * into 8-bit palette slots - a 5x8x5 colour cube in slots 0..199 - in one
 * of two SRAM buffers; core 1 shows the new buffer at the next vertical
 * blank, so a frame is never shown half drawn.  The driver scales the
 * picture to the screen (GRAPHICS_SCALE_FIT: square pixels where they fit).
 *
 * The launcher and the menus use the driver's text mode (CGA colours in
 * slots 200..215).
 */
#include "rp2350.h"

#include <string.h>

#include "pico/stdlib.h"
#include "hardware/sync.h"
#include "graphics.h"

#define CUBE_R 5
#define CUBE_G 8
#define CUBE_B 5

static uint8_t frames[2][NJ_MAX_PIXELS];
static uint8_t text_buf[80 * 30 * 2];

/* RGB555 -> cube slot: red and green from the low 10 bits, blue from the
 * high 5. */
static uint8_t lut_rg[1024];
static uint8_t lut_b[32];

static int canvas_w = 240, canvas_h = 320;

/* Written by core 0, applied by core 1 in the vertical blank. */
static volatile int  want_mode = TEXTMODE_DEFAULT;
static volatile int  want_buffer;          /* frame shown in game mode */
static volatile int  want_w = 240, want_h = 320;
static volatile uint32_t want_seq;         /* bumped on every request */
static volatile uint32_t done_seq;         /* the last request core 1 applied */
static volatile int  shown_buffer = -1;    /* frame on the screen now */
static int           back = 0;             /* frame core 0 draws next */

static uint8_t level(int i, int n)
{
    return (uint8_t)(i * 255 / (n - 1));
}

void video_core1_init(void)
{
    for (int i = 0; i < 1024; i++) {
        int r5 = i & 31, g5 = i >> 5;
        lut_rg[i] = (uint8_t)((r5 * CUBE_R >> 5) * (CUBE_G * CUBE_B) + (g5 * CUBE_G >> 5) * CUBE_B);
    }
    for (int i = 0; i < 32; i++)
        lut_b[i] = (uint8_t)(i * CUBE_B >> 5);

    graphics_init();
    for (int r = 0; r < CUBE_R; r++)
        for (int g = 0; g < CUBE_G; g++)
            for (int b = 0; b < CUBE_B; b++)
                graphics_set_palette((uint8_t)(r * CUBE_G * CUBE_B + g * CUBE_B + b),
                                     RGB888(level(r, CUBE_R), level(g, CUBE_G), level(b, CUBE_B)));
    graphics_set_textbuffer(text_buf);
    graphics_set_bgcolor(0x000000);
    graphics_set_offset(0, 0);
    graphics_set_flashmode(false, false);
    graphics_set_scale(GRAPHICS_SCALE_FIT);
    graphics_set_buffer(frames[0], (uint16_t)canvas_w, (uint16_t)canvas_h);
    graphics_set_mode(TEXTMODE_DEFAULT);
    done_seq = want_seq;
}

/* Core 1, once per frame (graphics_frame_count changed). */
void __not_in_flash_func(video_core1_vblank)(void)
{
    uint32_t seq = want_seq;
    if (seq == done_seq)
        return;
    __dmb();
    int buf = want_buffer;
    int w = want_w, h = want_h;
    graphics_set_buffer(frames[buf], (uint16_t)w, (uint16_t)h);
    if (want_mode == GRAPHICSMODE_DEFAULT)
        shown_buffer = buf;
    __dmb();
    done_seq = seq;
}

static void request(int mode, int buf, int w, int h)
{
    want_mode = mode;
    want_buffer = buf;
    want_w = w;
    want_h = h;
    __dmb();
    want_seq = want_seq + 1;
}

static void wait_applied(void)
{
    /* Core 1 applies requests at the next vertical blank (< 17 ms). */
    absolute_time_t until = make_timeout_time_ms(100);
    while (done_seq != want_seq && !time_reached(until))
        tight_loop_contents();
}

void video_text_mode(void)
{
    /* The text mode is switched directly: the driver reads the text buffer
     * line by line, there is no frame to tear. */
    graphics_set_mode(TEXTMODE_DEFAULT);
    want_mode = TEXTMODE_DEFAULT;
    video_clear_text();
}

static void fill_frame(uint8_t *f, int n)
{
    uint32_t *p = (uint32_t *)f;
    for (int i = 0; i < n / 4; i++)
        p[i] = 0;
}

void video_game_mode(int w, int h)
{
    if (w > NJ_MAX_W || h > NJ_MAX_H || w * h > NJ_MAX_PIXELS) {
        w = 240;
        h = 320;
    }
    canvas_w = w;
    canvas_h = h;
    fill_frame(frames[0], NJ_MAX_PIXELS);
    fill_frame(frames[1], NJ_MAX_PIXELS);
    back = 1;
    graphics_set_mode(GRAPHICSMODE_DEFAULT);
    request(GRAPHICSMODE_DEFAULT, 0, w, h);
    wait_applied();
}

void video_game_resume(void)
{
    int front = shown_buffer < 0 ? 0 : shown_buffer;
    graphics_set_mode(GRAPHICSMODE_DEFAULT);
    request(GRAPHICSMODE_DEFAULT, front, canvas_w, canvas_h);
    wait_applied();
}

void __not_in_flash_func(video_present)(const uint16_t *px, int w, int h)
{
    if (w != canvas_w || h != canvas_h || want_mode != GRAPHICSMODE_DEFAULT)
        return;
    /* Never draw into the frame on the screen: if the previous frame has
     * not been shown yet, draw over it (it is not on the screen either). */
    if (back == shown_buffer)
        back ^= 1;
    uint8_t *dst = frames[back];
    const uint32_t *src = (const uint32_t *)px;
    int n = w * h;
    if (((uintptr_t)px & 3) == 0) {
        for (int i = 0; i < n / 2; i++) {
            uint32_t two = src[i];
            uint32_t a = two & 0xFFFF, b = two >> 16;
            dst[2 * i] = (uint8_t)(lut_rg[a & 0x3FF] + lut_b[(a >> 10) & 31]);
            dst[2 * i + 1] = (uint8_t)(lut_rg[b & 0x3FF] + lut_b[(b >> 10) & 31]);
        }
        if (n & 1) {
            uint16_t c = px[n - 1];
            dst[n - 1] = (uint8_t)(lut_rg[c & 0x3FF] + lut_b[(c >> 10) & 31]);
        }
    } else {
        for (int i = 0; i < n; i++) {
            uint16_t c = px[i];
            dst[i] = (uint8_t)(lut_rg[c & 0x3FF] + lut_b[(c >> 10) & 31]);
        }
    }
    request(GRAPHICSMODE_DEFAULT, back, w, h);
    back ^= 1;
}

int video_text_cols(void)
{
    return TEXTMODE_COLS;
}

int video_text_rows(void)
{
    return TEXTMODE_ROWS;
}

void video_clear_text(void)
{
    for (size_t i = 0; i < sizeof text_buf; i += 2) {
        text_buf[i] = ' ';
        text_buf[i + 1] = 0x07;
    }
}

void video_text(int x, int y, const char *s, int fg, int bg)
{
    if (y < 0 || y >= TEXTMODE_ROWS || x >= TEXTMODE_COLS)
        return;
    draw_text(s, (uint32_t)x, (uint32_t)y, (uint8_t)fg, (uint8_t)bg);
}

void video_text_fill(int x, int y, int n, char c, int fg, int bg)
{
    if (y < 0 || y >= TEXTMODE_ROWS)
        return;
    for (int i = 0; i < n && x + i < TEXTMODE_COLS; i++) {
        uint8_t *cell = text_buf + (y * TEXTMODE_COLS + x + i) * 2;
        cell[0] = (uint8_t)c;
        cell[1] = (uint8_t)(bg << 4 | (fg & 15));
    }
}
