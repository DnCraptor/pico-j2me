/*
 * rp2350.h - shared declarations of the RP2350 (Murmulator) platform layer.
 *
 * Core 0 runs the launcher and the VM; core 1 runs the video driver (its
 * DMA interrupts), the keyboard / gamepad / USB host polling and the sound
 * mixer.  The cores talk through a few lock-free rings and flags.
 */
#ifndef NJ_RP2350_H
#define NJ_RP2350_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../../src/pal/pal.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NJ_HOME_DIR   "/j2me"               /* games */
#define NJ_CONFIG_DIR "/.config/pico-j2me"  /* settings, record stores */

/* ---- main.c ---- */

extern bool nj_sd_ok;                       /* the SD card is mounted */

/* Reboots through the safe path (PSRAM out of QPI, clocks back to normal). */
void nj_safe_reset(void) __attribute__((noreturn));

/* ---- memory (sys_rp2350.c) ---- */

void   sys_mem_init(void);                  /* after psram_init() */
bool   sys_psram(void);                     /* the native heap is in PSRAM */
size_t sys_psram_size(void);
/* Starts a fresh native heap for the next VM run (everything allocated
 * by the previous one is dropped at once). */
void   sys_mem_reset(void);
size_t sys_mem_free(void);                  /* the largest free block */
bool   sys_mkdirs(const char *dir);         /* creates every missing level */

/* ---- video (video.c) ---- */

/* The largest phone screen: two 8-bit frame buffers of this many pixels
 * live in SRAM. */
#define NJ_MAX_PIXELS (240 * 320)
#define NJ_MAX_W 320
#define NJ_MAX_H 320

void video_core1_init(void);                /* core 1: driver, palette */
void video_core1_vblank(void);              /* core 1: once per frame */
void video_text_mode(void);                 /* launcher, menus, errors */
void video_game_mode(int w, int h);         /* the phone screen, black */
/* Shows the game picture again after a menu (the frame is kept). */
void video_game_resume(void);
void video_present(const uint16_t *px, int w, int h);
int  video_text_cols(void);
int  video_text_rows(void);
void video_clear_text(void);
/* Text mode: fg/bg are 0..15 (the CGA colours). */
void video_text(int x, int y, const char *s, int fg, int bg);
void video_text_fill(int x, int y, int n, char c, int fg, int bg);

/* ---- input (input.cpp) ---- */

/* Raw input: what the keyboard (PS/2 or USB) and the NES/SNES pad did,
 * queued by core 1, read by core 0. */
enum { RAW_KEY, RAW_PAD };
typedef struct RawInput {
    uint8_t  kind;      /* RAW_KEY: HID usage (modifiers as 0xE0..0xE7); RAW_PAD: bit */
    uint8_t  code;
    bool     down;
} RawInput;

void input_init(void);                      /* core 0, before core 1 starts */
void input_core1_init(void);
void input_core1_poll(void);                /* core 1 main loop */
bool input_get(RawInput *in);               /* core 0 */
bool input_pending(void);
void input_flush(void);
bool input_shift_held(void);

/* NES / SNES pad bits as RAW_PAD codes */
enum {
    PAD_A, PAD_B, PAD_SELECT, PAD_START, PAD_UP, PAD_DOWN, PAD_LEFT, PAD_RIGHT,
    PAD_X, PAD_Y, PAD_L, PAD_R, PAD_COUNT
};

/* Navigation keys for the launcher and the menus. */
enum {
    UI_NONE, UI_UP, UI_DOWN, UI_LEFT, UI_RIGHT, UI_OK, UI_BACK,
    UI_PAGE_UP, UI_PAGE_DOWN, UI_HOME, UI_END, UI_MENU
};
/* Waits for the next navigation key (with auto-repeat for held arrows). */
int  ui_key_wait(void);

/* ---- sound (audio_rp2350.c) ---- */

void audio_core1_init(void);
void audio_core1_poll(void);
void audio_pause(bool paused);              /* silence while a menu is open */
void audio_reset(void);                     /* stop everything (VM shutdown) */
void audio_master(int volume);              /* 0-100 */

/* ---- per-game settings (settings.c) ---- */

typedef struct GameSettings {
    int w, h;          /* phone screen */
    int volume;        /* 0-100 */
} GameSettings;

extern const int nj_screen_sizes[][2];
extern const int nj_num_screen_sizes;

void settings_defaults(GameSettings *s);
void settings_load(const char *jar, GameSettings *s);
void settings_save(const char *jar, const GameSettings *s);
/* "/j2me/Game.jar" -> "Game" (no directories, no extension) */
void settings_game_name(const char *path, char *out, size_t size);

/* ---- launcher and menus (ui.c) ---- */

/* Lets the user pick a .jar / .jad; returns a malloc'd path. */
char *ui_launcher(void);
/* The menu opened during a game (F12 / Select).  Returns an EXIT_*. */
enum { EXIT_NONE, EXIT_RESTART_GAME, EXIT_CHANGE_GAME };
int  ui_game_menu(GameSettings *s, const char *jar);
/* Shows a message (and the end of the log) until a key is pressed. */
void ui_message(const char *title, const char *msg, bool show_log);

/* ---- platform (pal_rp2350.c) ---- */

/* Starts a game's video and input with these settings. */
void pal_session_start(GameSettings *s, const char *jar);
/* Copies the end of the console log (lines) into out. */
void pal_log_tail(char *out, size_t size, int lines);
/* What the game menu asked for when the VM stopped. */
extern int nj_exit_action;

#ifdef __cplusplus
}
#endif

#endif
