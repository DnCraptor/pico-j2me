/*
 * settings.c - per-game settings in /.config/pico-j2me/<game>.ini:
 *
 *   screen=176x208     phone screen size (only when it is not the default)
 *   volume=80          J2ME volume, 0-100
 *
 * The default screen is the game's original one when its .jad or manifest
 * declares it (Nokia-MIDlet-Original-Display-Size and friends), else
 * 240x320.  A screen that is not one of nj_screen_sizes is shown through
 * the nearest of them (crop-to-view): the game draws its own size, the
 * view is centred on it, whatever falls outside the view is cut off and
 * the view's margins beyond the game's picture stay black.
 */
#include "rp2350.h"
#include "../../src/util/zip.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "ff.h"

const int nj_screen_sizes[][2] = {
    {128, 128}, {128, 160}, {176, 208}, {176, 220}, {208, 208},
    {240, 240}, {240, 320}, {320, 240},
};
const int nj_num_screen_sizes = sizeof nj_screen_sizes / sizeof nj_screen_sizes[0];

#define DEFAULT_W 240
#define DEFAULT_H 320

/* What the game sees: a real phone of the size it was made for. */
static bool valid_size(int w, int h)
{
    return w >= 32 && h >= 32 && w <= NJ_MAX_CANVAS && h <= NJ_MAX_CANVAS;
}

bool settings_supported(int w, int h)
{
    for (int i = 0; i < nj_num_screen_sizes; i++)
        if (nj_screen_sizes[i][0] == w && nj_screen_sizes[i][1] == h)
            return true;
    return false;
}

void settings_set_screen(GameSettings *s, int w, int h)
{
    s->w = w;
    s->h = h;
    if (settings_supported(w, h)) {
        s->view_w = w;
        s->view_h = h;
        return;
    }
    /* crop-to-view: the nearest supported size; on a tie the one that
     * shows the whole picture */
    int best = -1, best_d = 0;
    bool best_covers = false;
    for (int i = 0; i < nj_num_screen_sizes; i++) {
        int vw = nj_screen_sizes[i][0], vh = nj_screen_sizes[i][1];
        int d = abs(vw - w) + abs(vh - h);
        bool covers = vw >= w && vh >= h;
        if (best < 0 || d < best_d || (d == best_d && covers && !best_covers)) {
            best = i;
            best_d = d;
            best_covers = covers;
        }
    }
    s->view_w = nj_screen_sizes[best][0];
    s->view_h = nj_screen_sizes[best][1];
}

void settings_defaults(GameSettings *s)
{
    s->orig_w = s->orig_h = 0;
    settings_set_screen(s, DEFAULT_W, DEFAULT_H);
    s->volume = 80;
}

void settings_default_screen(const GameSettings *s, int *w, int *h)
{
    *w = s->orig_w ? s->orig_w : DEFAULT_W;
    *h = s->orig_w ? s->orig_h : DEFAULT_H;
}

/* ------------------------------------------------------------------------ */
/* The original screen size, from the .jad and the jar's manifest           */

static const char *const size_keys[] = {
    "Nokia-MIDlet-Original-Display-Size",
    "Nokia-MIDlet-Target-Display-Size",
    "MIDlet-Display-Size",
};

/* Looks for the size keys in "Key: value" text; true if one is valid. */
static bool parse_size(const char *text, int *w, int *h)
{
    for (size_t k = 0; k < sizeof size_keys / sizeof size_keys[0]; k++) {
        size_t n = strlen(size_keys[k]);
        for (const char *p = text; *p;) {
            const char *eol = strchr(p, '\n');
            const char *line = p;
            p = eol ? eol + 1 : line + strlen(line);
            if (strncasecmp(line, size_keys[k], n) != 0)
                continue;
            const char *v = line + n;
            while (*v == ' ' || *v == '\t')
                v++;
            if (*v != ':')
                continue;
            v++;
            int a = 0, b = 0;
            char sep = 0;
            if (sscanf(v, " %d %c %d", &a, &sep, &b) == 3 &&
                (sep == ',' || sep == 'x' || sep == 'X' || sep == '*') && valid_size(a, b)) {
                *w = a;
                *h = b;
                return true;
            }
        }
    }
    return false;
}

static char *read_text(const char *path)
{
    FIL f;
    if (f_open(&f, path, FA_READ) != FR_OK)
        return NULL;
    UINT size = (UINT)f_size(&f), got = 0;
    if (size > 16384)
        size = 16384;   /* a .jad is small; the keys are near the top */
    char *t = malloc(size + 1);
    if (t && f_read(&f, t, size, &got) == FR_OK) {
        t[got] = 0;
    } else {
        free(t);
        t = NULL;
    }
    f_close(&f);
    return t;
}

static bool manifest_size(const char *jar, int *w, int *h)
{
    Zip *z = zip_open_file(jar);
    if (!z)
        return false;
    uint32_t len;
    char *mf = (char *)zip_read_name(z, "META-INF/MANIFEST.MF", &len);
    bool  ok = mf && parse_size(mf, w, h);
    pal_free(mf);
    zip_close(z);
    return ok;
}

/* The jar next to a .jad: MIDlet-Jar-URL (its file name), else <name>.jar */
static void jad_jar(const char *jad_path, const char *jad, char *out, size_t size)
{
    snprintf(out, size, "%s", jad_path);
    char *base = strrchr(out, '/');
    base = base ? base + 1 : out;
    const char *url = jad ? strstr(jad, "MIDlet-Jar-URL:") : NULL;
    if (url) {
        url += strlen("MIDlet-Jar-URL:");
        while (*url == ' ' || *url == '\t')
            url++;
        size_t n = strcspn(url, "\r\n");
        const char *name = url;
        for (const char *p = url; p < url + n; p++)
            if (*p == '/')
                name = p + 1;
        n -= (size_t)(name - url);
        FILINFO fi;
        if (n > 0 && (size_t)(base - out) + n < size) {
            memcpy(base, name, n);
            base[n] = 0;
            if (f_stat(out, &fi) == FR_OK)
                return;
        }
    }
    snprintf(out, size, "%s", jad_path);
    char *dot = strrchr(out, '.');
    if (dot && strlen(dot) == 4)
        strcpy(dot, ".jar");
}

static bool original_size(const char *path, int *w, int *h)
{
    /* the launcher asks again on every redraw: remember the last game */
    static char cached[256];
    static int  cached_w, cached_h;
    static bool cached_ok;
    if (strcmp(cached, path) == 0) {
        *w = cached_w;
        *h = cached_h;
        return cached_ok;
    }
    bool ok = false;
    size_t n = strlen(path);
    if (n > 4 && strcasecmp(path + n - 4, ".jad") == 0) {
        char *jad = read_text(path);
        ok = jad && parse_size(jad, w, h);
        if (!ok) {
            char jar[256];
            jad_jar(path, jad, jar, sizeof jar);
            ok = manifest_size(jar, w, h);
        }
        free(jad);
    } else {
        ok = manifest_size(path, w, h);
    }
    snprintf(cached, sizeof cached, "%s", path);
    cached_ok = ok;
    cached_w = ok ? *w : 0;
    cached_h = ok ? *h : 0;
    return ok;
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

void settings_load(const char *jar, GameSettings *s)
{
    settings_defaults(s);
    if (!jar || !nj_sd_ok)
        return;
    int ow, oh;
    if (original_size(jar, &ow, &oh)) {
        s->orig_w = ow;
        s->orig_h = oh;
        settings_set_screen(s, ow, oh);
    }
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
            settings_set_screen(s, w, h);
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
    /* the default screen is not written: it follows the game */
    int dw, dh, n = 0;
    settings_default_screen(s, &dw, &dh);
    if (s->w != dw || s->h != dh)
        n = snprintf(text, sizeof text, "screen=%dx%d\n", s->w, s->h);
    n += snprintf(text + n, sizeof text - (size_t)n, "volume=%d\n", s->volume);
    UINT put;
    f_write(&f, text, (UINT)n, &put);
    f_close(&f);
}
