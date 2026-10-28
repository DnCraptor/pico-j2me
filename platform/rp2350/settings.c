/*
 * settings.c - per-game settings in /.config/pico-j2me/<game>.ini:
 *
 *   screen=176x208     phone screen size
 *   volume=80          J2ME volume, 0-100
 */
#include "rp2350.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ff.h"

const int nj_screen_sizes[][2] = {
    {128, 128}, {128, 160}, {176, 208}, {176, 220}, {208, 208},
    {240, 240}, {240, 320}, {320, 240},
};
const int nj_num_screen_sizes = sizeof nj_screen_sizes / sizeof nj_screen_sizes[0];

void settings_defaults(GameSettings *s)
{
    s->w = 240;
    s->h = 320;
    s->volume = 80;
}

void settings_game_name(const char *path, char *out, size_t size)
{
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    snprintf(out, size, "%s", base);
    char *dot = strrchr(out, '.');
    if (dot && dot != out)
        *dot = 0;
}

static void ini_path(const char *jar, char *out, size_t size)
{
    char name[128];
    settings_game_name(jar, name, sizeof name);
    snprintf(out, size, NJ_CONFIG_DIR "/%s.ini", name);
}

static bool valid_size(int w, int h)
{
    return w >= 64 && h >= 64 && w <= NJ_MAX_W && h <= NJ_MAX_H && w * h <= NJ_MAX_PIXELS;
}

void settings_load(const char *jar, GameSettings *s)
{
    settings_defaults(s);
    if (!jar || !nj_sd_ok)
        return;
    char path[256], line[128];
    ini_path(jar, path, sizeof path);
    FIL f;
    if (f_open(&f, path, FA_READ) != FR_OK)
        return;
    while (f_gets(line, sizeof line, &f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *eq = strchr(line, '=');
        if (!eq)
            continue;
        *eq = 0;
        const char *key = line, *val = eq + 1;
        int w, h;
        if (strcmp(key, "screen") == 0 && sscanf(val, "%dx%d", &w, &h) == 2 && valid_size(w, h)) {
            s->w = w;
            s->h = h;
        } else if (strcmp(key, "volume") == 0) {
            int v = atoi(val);
            s->volume = v < 0 ? 0 : v > 100 ? 100 : v;
        }
    }
    f_close(&f);
}

void settings_save(const char *jar, const GameSettings *s)
{
    if (!jar || !nj_sd_ok)
        return;
    char path[256], text[64];
    ini_path(jar, path, sizeof path);
    sys_mkdirs(NJ_CONFIG_DIR);
    FIL f;
    if (f_open(&f, path, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK)
        return;
    int n = snprintf(text, sizeof text, "screen=%dx%d\nvolume=%d\n", s->w, s->h, s->volume);
    UINT put;
    f_write(&f, text, (UINT)n, &put);
    f_close(&f);
}
