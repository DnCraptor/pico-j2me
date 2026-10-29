/*
 * video_tft.c - the screen on an SPI TFT panel (ST7789 or ILI9341, 320x240)
 * driven by a PIO serial program (drivers/st7789, from pico-spec).
 *
 * The same picture path as the VGA / HDMI build (video.c): pal_present()
 * turns each 15-bit frame into slots of the 5x8x5 colour cube in one of two
 * 8-bit SRAM buffers.  Core 1 sends the newest finished buffer to the panel:
 * one line at a time through a 16-bit line buffer (the palette lookup) and
 * DMA, so its main loop keeps polling input and sound between lines.
 * Frames that arrive while one is being sent are dropped (only the newest
 * is sent next).
 *
 * The panel has no scaling: the phone screen is shown 1:1 in the middle.
 * Screens up to 320x240 use the panel in landscape, taller ones (240x320)
 * in portrait - the panel is rotated by its controller (MADCTL), so the
 * board is turned by 90 degrees for those games (TFT_PORTRAIT_FLIP picks
 * the direction).  The launcher and the menus are 53x30 text cells of
 * 6x8 pixels in landscape.
 *
 * All panel traffic happens on core 1; core 0 only posts requests.
 */
#include "rp2350.h"

#include <string.h>

#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/pio.h"
#include "hardware/sync.h"

#include "st7789.pio.h"
#include "../../drivers/graphics/font6x8.h"

#ifndef TFT_MAX_MHZ
#define TFT_MAX_MHZ 126   /* PIO clock limit: the SPI clock is half of it */
#endif

#define PANEL_LONG  320
#define PANEL_SHORT 240

#define TEXT_COLS 53      /* 53 * 6 = 318 of 320 pixels */
#define TEXT_ROWS 30

/* ---- MADCTL ------------------------------------------------------------ */

#define MADCTL_MY  0x80
#define MADCTL_MX  0x40
#define MADCTL_MV  0x20
#define MADCTL_BGR 0x08

/* Landscape as in pico-spec; portrait is the neighbouring rotation of the
 * same controller (rotation 0, or rotation 2 with TFT_PORTRAIT_FLIP). */
#ifdef ILI9341
#define MADCTL_LANDSCAPE (MADCTL_MV | MADCTL_BGR)
#ifdef TFT_PORTRAIT_FLIP
#define MADCTL_PORTRAIT  (MADCTL_MY | MADCTL_BGR)
#else
#define MADCTL_PORTRAIT  (MADCTL_MX | MADCTL_BGR)
#endif
/* ILI9341: RGB565 */
#define RGB565(r, g, b) ((uint16_t)((((r) >> 3) << 11) | (((g) >> 2) << 5) | ((b) >> 3)))
#else
#define MADCTL_LANDSCAPE (MADCTL_MX | MADCTL_MV | MADCTL_BGR)
#ifdef TFT_PORTRAIT_FLIP
#define MADCTL_PORTRAIT  (MADCTL_MX | MADCTL_MY | MADCTL_BGR)
#else
#define MADCTL_PORTRAIT  (MADCTL_BGR)
#endif
/* ST7789 with MADCTL BGR: BGR565 */
#define RGB565(r, g, b) ((uint16_t)((((b) >> 3) << 11) | (((g) >> 2) << 5) | ((r) >> 3)))
#endif

#ifdef TFT_INV
#define TFT_INVERSION 1   /* ST7789V and other panels that need INVON */
#else
#define TFT_INVERSION 0
#endif

/* ---- picture ----------------------------------------------------------- */

#define CUBE_R 5
#define CUBE_G 8
#define CUBE_B 5
#define TEXT_PAL 200      /* the 16 CGA colours of the text mode */

static uint8_t  frames[2][NJ_MAX_PIXELS];
static uint8_t  text_buf[TEXT_COLS * TEXT_ROWS * 2];
static uint16_t pal[256];
static uint16_t lines[2][PANEL_LONG];

/* RGB555 -> cube slot (as in video.c) */
static uint8_t lut_rg[1024];
static uint8_t lut_b[32];

static int canvas_w = 240, canvas_h = 320;   /* what the game draws */
static int view_w = 240, view_h = 320;       /* what the frame buffers hold */
static int src_x, src_y, dst_x, dst_y, copy_w, copy_h;

enum { MODE_TEXT, MODE_GAME };

/* Written by core 0, taken by core 1 when it is idle. */
static volatile int      want_mode = MODE_TEXT;
static volatile int      want_buffer;
static volatile int      want_w = 240, want_h = 320;
static volatile uint32_t want_seq;
static volatile uint32_t done_seq;
static volatile bool     text_dirty;
static volatile int      shown_buffer = -1;   /* the frame being sent / sent last */
static int               back;                /* the frame core 0 draws next */

/* ---- the panel (core 1) ------------------------------------------------ */

static PIO  pio = pio0;
static int  panel_madctl = -1;   /* the MADCTL value sent last */
static uint sm;
static uint dma_chan;

static inline void lcd_set_dc_cs(bool dc, bool cs)
{
    sleep_us(5);
    gpio_put_masked((1u << TFT_DC_PIN) | (1u << TFT_CS_PIN),
                    (uint32_t)dc << TFT_DC_PIN | (uint32_t)cs << TFT_CS_PIN);
    sleep_us(5);
}

static void lcd_write_cmd(const uint8_t *cmd, size_t count)
{
    st7789_lcd_wait_idle(pio, sm);
    lcd_set_dc_cs(0, 0);
    st7789_lcd_put(pio, sm, *cmd++);
    if (count >= 2) {
        st7789_lcd_wait_idle(pio, sm);
        lcd_set_dc_cs(1, 0);
        for (size_t i = 0; i < count - 1; ++i)
            st7789_lcd_put(pio, sm, *cmd++);
    }
    st7789_lcd_wait_idle(pio, sm);
    lcd_set_dc_cs(1, 1);
}

static void lcd_cmd1(uint8_t cmd, uint8_t arg)
{
    const uint8_t c[2] = {cmd, arg};
    lcd_write_cmd(c, 2);
}

static void lcd_set_window(int x, int y, int w, int h)
{
    const int x1 = x + w - 1, y1 = y + h - 1;
    const uint8_t caset[5] = {0x2A, (uint8_t)(x >> 8), (uint8_t)x, (uint8_t)(x1 >> 8), (uint8_t)x1};
    const uint8_t raset[5] = {0x2B, (uint8_t)(y >> 8), (uint8_t)y, (uint8_t)(y1 >> 8), (uint8_t)y1};
    lcd_write_cmd(caset, 5);
    lcd_write_cmd(raset, 5);
}

static void start_pixels(void)
{
    const uint8_t cmd = 0x2C;   /* RAMWR */
    st7789_lcd_wait_idle(pio, sm);
    st7789_set_pixel_mode(pio, sm, false);
    lcd_write_cmd(&cmd, 1);
    st7789_set_pixel_mode(pio, sm, true);
    lcd_set_dc_cs(1, 0);
}

static void stop_pixels(void)
{
    st7789_lcd_wait_idle(pio, sm);
    lcd_set_dc_cs(1, 1);
    st7789_set_pixel_mode(pio, sm, false);
}

static void lcd_init(void)
{
    const uint offset = pio_add_program(pio, &st7789_lcd_program);
    sm = pio_claim_unused_sm(pio, true);
    float clk_div = (float)clock_get_hz(clk_sys) / (TFT_MAX_MHZ * 1000000.0f);
    if (clk_div < 1.0f)
        clk_div = 1.0f;
    st7789_lcd_program_init(pio, sm, offset, TFT_DATA_PIN, TFT_CLK_PIN, clk_div);

    gpio_init(TFT_CS_PIN);
    gpio_init(TFT_DC_PIN);
    gpio_init(TFT_RST_PIN);
    gpio_init(TFT_LED_PIN);
    gpio_set_dir(TFT_CS_PIN, GPIO_OUT);
    gpio_set_dir(TFT_DC_PIN, GPIO_OUT);
    gpio_set_dir(TFT_RST_PIN, GPIO_OUT);
    gpio_set_dir(TFT_LED_PIN, GPIO_OUT);
    gpio_put(TFT_CS_PIN, 1);
    gpio_put(TFT_RST_PIN, 1);

    /* data size (command included), delay in 5 ms units, command, data */
    static const uint8_t init_seq[] = {
        1, 20, 0x01,                      /* software reset */
        1, 10, 0x11,                      /* sleep out */
        2, 2, 0x3A, 0x55,                 /* 16-bit colour */
        1, 2, 0x20 | TFT_INVERSION,       /* inversion off / on */
        2, 0, 0x36, MADCTL_LANDSCAPE,
        1, 2, 0x13,                       /* normal display mode */
        1, 2, 0x29,                       /* display on */
        0
    };
    for (const uint8_t *c = init_seq; *c; c += *c + 2) {
        lcd_write_cmd(c + 2, *c);
        sleep_ms(c[1] * 5);
    }
    panel_madctl = MADCTL_LANDSCAPE;
    gpio_put(TFT_LED_PIN, 1);

    dma_chan = (uint)dma_claim_unused_channel(true);
    dma_channel_config c = dma_channel_get_default_config(dma_chan);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_16);
    channel_config_set_dreq(&c, pio_get_dreq(pio, sm, true));
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, false);
    dma_channel_configure(dma_chan, &c, &pio->txf[sm], NULL, 0, false);
}

/* ---- sending a picture: one line per step (core 1) ---------------------- */

enum { JOB_NONE, JOB_BLACK, JOB_FRAME, JOB_TEXT };

static int job;              /* what is being sent */
static int job_w, job_h;     /* the window */
static int job_y;            /* the line in lines[job_cur] (next to start) */
static int job_cur;
static int job_buf;          /* JOB_FRAME: the frame buffer */
static bool need_clear;      /* the layout changed: black panel first */
static bool frame_pending;   /* a game frame was taken, not sent yet */

static void __not_in_flash_func(fill_line)(uint16_t *dst, int y)
{
    switch (job) {
    case JOB_FRAME: {
        const uint8_t *src = frames[job_buf] + y * job_w;
        for (int x = 0; x < job_w; x++)
            dst[x] = pal[src[x]];
        break;
    }
    case JOB_TEXT: {
        const uint8_t *cells = text_buf + (y >> 3) * TEXT_COLS * 2;
        const int gy = y & 7;
        uint16_t *out = dst;
        for (int col = 0; col < TEXT_COLS; col++) {
            const uint8_t ch = cells[col * 2], attr = cells[col * 2 + 1];
            const uint16_t fg = pal[TEXT_PAL + (attr & 15)], bg = pal[TEXT_PAL + (attr >> 4)];
            uint8_t bits = font_6x8[ch * 8 + gy];
            for (int b = 0; b < 6; b++, bits >>= 1)
                *out++ = (bits & 1) ? fg : bg;
        }
        while (out < dst + job_w)
            *out++ = 0;
        break;
    }
    default:
        for (int x = 0; x < job_w; x++)
            dst[x] = 0;
        break;
    }
}

static void set_orientation(int madctl)
{
    if (madctl != panel_madctl) {
        lcd_cmd1(0x36, (uint8_t)madctl);
        panel_madctl = madctl;
    }
}

static void job_start(int kind, int x, int y, int w, int h)
{
    job = kind;
    job_w = w;
    job_h = h;
    lcd_set_window(x, y, w, h);
    start_pixels();
    job_cur = 0;
    job_y = 0;
    fill_line(lines[0], 0);
}

/* Sends the next line if the previous one is out; true while busy. */
static bool __not_in_flash_func(job_step)(void)
{
    if (job == JOB_NONE)
        return false;
    if (dma_channel_is_busy(dma_chan))
        return true;
    if (job_y == job_h) {
        stop_pixels();
        job = JOB_NONE;
        return false;
    }
    dma_channel_transfer_from_buffer_now(dma_chan, lines[job_cur], (uint)job_w);
    job_y++;
    job_cur ^= 1;
    if (job_y < job_h)
        fill_line(lines[job_cur], job_y);   /* while the DMA sends the other */
    return true;
}

static int cur_mode = -1, cur_w, cur_h;

static bool landscape(int w, int h)
{
    return w <= PANEL_LONG && h <= PANEL_SHORT;
}

/* Core 1 main loop. */
void __not_in_flash_func(video_core1_poll)(void)
{
    if (job_step())
        return;

    uint32_t seq = want_seq;
    if (seq != done_seq) {
        __dmb();
        int mode = want_mode, buf = want_buffer, w = want_w, h = want_h;
        if (mode != cur_mode || w != cur_w || h != cur_h)
            need_clear = mode == MODE_GAME;
        cur_mode = mode;
        cur_w = w;
        cur_h = h;
        if (mode == MODE_GAME) {
            set_orientation(landscape(w, h) ? MADCTL_LANDSCAPE : MADCTL_PORTRAIT);
            job_buf = buf;
            shown_buffer = buf;
            frame_pending = true;
        } else {
            frame_pending = false;
            set_orientation(MADCTL_LANDSCAPE);
            text_dirty = true;
        }
        __dmb();
        done_seq = seq;
    }

    if (cur_mode == MODE_GAME) {
        const bool land = landscape(cur_w, cur_h);
        const int pw = land ? PANEL_LONG : PANEL_SHORT, ph = land ? PANEL_SHORT : PANEL_LONG;
        if (need_clear) {
            need_clear = false;
            job_start(JOB_BLACK, 0, 0, pw, ph);
            return;
        }
        if (frame_pending) {
            frame_pending = false;
            job_start(JOB_FRAME, (pw - cur_w) / 2, (ph - cur_h) / 2, cur_w, cur_h);
        }
    } else if (cur_mode == MODE_TEXT && text_dirty) {
        text_dirty = false;
        job_start(JOB_TEXT, 0, 0, PANEL_LONG, PANEL_SHORT);
    }
}

/* ---- the interface (core 0 unless noted) -------------------------------- */

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
    for (int r = 0; r < CUBE_R; r++)
        for (int g = 0; g < CUBE_G; g++)
            for (int b = 0; b < CUBE_B; b++)
                pal[r * CUBE_G * CUBE_B + g * CUBE_B + b] =
                    RGB565(level(r, CUBE_R), level(g, CUBE_G), level(b, CUBE_B));
    static const uint32_t cga[16] = {
        0x000000, 0x0000C4, 0x00C400, 0x00C4C4, 0xC40000, 0xC400C4, 0xC47E00, 0xC4C4C4,
        0x4E4E4E, 0x4E4EDC, 0x4EDC4E, 0x4EF3F3, 0xDC4E4E, 0xF34EF3, 0xF3F34E, 0xFFFFFF,
    };
    for (int i = 0; i < 16; i++)
        pal[TEXT_PAL + i] = RGB565(cga[i] >> 16, (cga[i] >> 8) & 0xFF, cga[i] & 0xFF);

    video_clear_text();
    lcd_init();
    /* the first picture: the (empty) text screen */
    cur_mode = MODE_TEXT;
    text_dirty = true;
    done_seq = want_seq;
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
    /* core 1 takes a request when the picture it is sending is out (<25 ms) */
    absolute_time_t until = make_timeout_time_ms(100);
    while (done_seq != want_seq && !time_reached(until))
        tight_loop_contents();
}

void video_text_mode(void)
{
    video_clear_text();
    request(MODE_TEXT, 0, view_w, view_h);
    wait_applied();
}

static void fill_frame(uint8_t *f, int n)
{
    uint32_t *p = (uint32_t *)f;
    for (int i = 0; i < n / 4; i++)
        p[i] = 0;
}

/* One axis of the crop: the canvas range [*src, *src + *n) goes to the
 * view from *dst. */
static void crop_axis(int canvas, int view, int *src, int *dst, int *n)
{
    if (canvas >= view) {
        *src = (canvas - view) / 2;
        *dst = 0;
        *n = view;
    } else {
        *src = 0;
        *dst = (view - canvas) / 2;
        *n = canvas;
    }
}

void video_game_mode(int w, int h, int vw, int vh)
{
    bool fits = (vw <= PANEL_LONG && vh <= PANEL_SHORT) || (vw <= PANEL_SHORT && vh <= PANEL_LONG);
    if (!fits || vw * vh > NJ_MAX_PIXELS || vw < 1 || vh < 1) {
        vw = 240;
        vh = 320;
    }
    canvas_w = w;
    canvas_h = h;
    view_w = vw;
    view_h = vh;
    crop_axis(w, vw, &src_x, &dst_x, &copy_w);
    crop_axis(h, vh, &src_y, &dst_y, &copy_h);
    /* core 1 may still be sending the text screen: frames are not in use */
    wait_applied();
    fill_frame(frames[0], NJ_MAX_PIXELS);
    fill_frame(frames[1], NJ_MAX_PIXELS);
    back = 1;
    shown_buffer = -1;
    request(MODE_GAME, 0, vw, vh);
    wait_applied();
}

void video_game_resume(void)
{
    int front = shown_buffer < 0 ? 0 : shown_buffer;
    request(MODE_GAME, front, view_w, view_h);
    wait_applied();
}

static inline uint8_t slot(uint32_t c)
{
    return (uint8_t)(lut_rg[c & 0x3FF] + lut_b[(c >> 10) & 31]);
}

static void __not_in_flash_func(convert_row)(uint8_t *dst, const uint16_t *src, int n)
{
    int i = 0;
    if ((uintptr_t)src & 2) {
        if (n > 0)
            dst[i] = slot(src[i]);
        i = 1;
    }
    for (; i + 1 < n; i += 2) {
        uint32_t two = *(const uint32_t *)(src + i);
        dst[i] = slot(two & 0xFFFF);
        dst[i + 1] = slot(two >> 16);
    }
    if (i < n)
        dst[i] = slot(src[i]);
}

void __not_in_flash_func(video_present)(const uint16_t *px, int w, int h)
{
    if (w != canvas_w || h != canvas_h || want_mode != MODE_GAME)
        return;
    /* never draw into the frame core 1 is sending */
    if (back == shown_buffer)
        back ^= 1;
    uint8_t *dst = frames[back];
    if (copy_w == w && copy_w == view_w && copy_h == h) {
        convert_row(dst, px, w * h);
    } else {
        for (int y = 0; y < copy_h; y++)
            convert_row(dst + (dst_y + y) * view_w + dst_x, px + (src_y + y) * w + src_x, copy_w);
    }
    request(MODE_GAME, back, view_w, view_h);
    back ^= 1;
}

int video_text_cols(void)
{
    return TEXT_COLS;
}

int video_text_rows(void)
{
    return TEXT_ROWS;
}

void video_clear_text(void)
{
    for (size_t i = 0; i < sizeof text_buf; i += 2) {
        text_buf[i] = ' ';
        text_buf[i + 1] = 0x07;
    }
    text_dirty = true;
}

void video_text(int x, int y, const char *s, int fg, int bg)
{
    if (y < 0 || y >= TEXT_ROWS || x < 0)
        return;
    uint8_t *cell = text_buf + (y * TEXT_COLS + x) * 2;
    for (int i = x; i < TEXT_COLS && *s; i++) {
        *cell++ = (uint8_t)*s++;
        *cell++ = (uint8_t)(bg << 4 | (fg & 15));
    }
    text_dirty = true;
}

void video_text_fill(int x, int y, int n, char c, int fg, int bg)
{
    if (y < 0 || y >= TEXT_ROWS || x < 0)
        return;
    for (int i = 0; i < n && x + i < TEXT_COLS; i++) {
        uint8_t *cell = text_buf + (y * TEXT_COLS + x + i) * 2;
        cell[0] = (uint8_t)c;
        cell[1] = (uint8_t)(bg << 4 | (fg & 15));
    }
    text_dirty = true;
}
