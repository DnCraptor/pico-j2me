/*
 * ui.c - the launcher (games on the SD card), the emulator menu and the
 * message screen, all in the video driver's text mode.
 */
#include "rp2350.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "pico/stdlib.h"
#include "tusb.h"   /* HID_KEY_* */
#include "ff.h"

#ifndef NJ_VERSION
#define NJ_VERSION "dev"
#endif

/* CGA colours of the text mode */
enum {
    C_BLACK, C_BLUE, C_GREEN, C_CYAN, C_RED, C_MAGENTA, C_BROWN, C_GRAY,
    C_DARK, C_LBLUE, C_LGREEN, C_LCYAN, C_LRED, C_LMAGENTA, C_YELLOW, C_WHITE
};

/* ------------------------------------------------------------------------ */
/* Navigation keys                                                          */

static int ui_code(const RawInput *in)
{
    if (in->kind == RAW_PAD) {
        switch (in->code) {
        case PAD_UP:     return UI_UP;
        case PAD_DOWN:   return UI_DOWN;
        case PAD_LEFT:   return UI_LEFT;
        case PAD_RIGHT:  return UI_RIGHT;
        case PAD_A:      case PAD_START: return UI_OK;
        case PAD_B:      return UI_BACK;
        case PAD_SELECT: return UI_MENU;
        case PAD_L:      return UI_PAGE_UP;
        case PAD_R:      return UI_PAGE_DOWN;
        default:         return UI_NONE;
        }
    }
    switch (in->code) {
    case HID_KEY_ARROW_UP:    case HID_KEY_KEYPAD_8: return UI_UP;
    case HID_KEY_ARROW_DOWN:  case HID_KEY_KEYPAD_2: return UI_DOWN;
    case HID_KEY_ARROW_LEFT:  case HID_KEY_KEYPAD_4: return UI_LEFT;
    case HID_KEY_ARROW_RIGHT: case HID_KEY_KEYPAD_6: return UI_RIGHT;
    case HID_KEY_ENTER:       case HID_KEY_KEYPAD_ENTER: case HID_KEY_SPACE: return UI_OK;
    case HID_KEY_ESCAPE:      case HID_KEY_BACKSPACE: return UI_BACK;
    case HID_KEY_PAGE_UP:     case HID_KEY_KEYPAD_9: return UI_PAGE_UP;
    case HID_KEY_PAGE_DOWN:   case HID_KEY_KEYPAD_3: return UI_PAGE_DOWN;
    case HID_KEY_HOME:        case HID_KEY_KEYPAD_7: return UI_HOME;
    case HID_KEY_END:         case HID_KEY_KEYPAD_1: return UI_END;
    case HID_KEY_F12:         return UI_MENU;
    default:                  return UI_NONE;
    }
}

#define REPEAT_DELAY_US 400000
#define REPEAT_RATE_US  70000

static int      held_key;      /* a navigation key being held, for repeat */
static uint8_t  held_kind, held_code;
static uint64_t next_repeat;

int ui_key_wait(void)
{
    for (;;) {
        RawInput in;
        while (input_get(&in)) {
            int k = ui_code(&in);
            if (in.down && k != UI_NONE) {
                held_key = k;
                held_kind = in.kind;
                held_code = in.code;
                next_repeat = time_us_64() + REPEAT_DELAY_US;
                return k;
            }
            if (!in.down && held_key && in.kind == held_kind && in.code == held_code)
                held_key = UI_NONE;
        }
        if (held_key != UI_NONE && held_key != UI_OK && held_key != UI_BACK &&
            held_key != UI_MENU && time_us_64() >= next_repeat) {
            next_repeat = time_us_64() + REPEAT_RATE_US;
            return held_key;
        }
        sleep_ms(5);
    }
}

/* Forgets what is held and queued (after a game, before a menu). */
static void ui_keys_reset(void)
{
    input_flush();
    held_key = UI_NONE;   /* a key still held from the game must not repeat */
}

/* ------------------------------------------------------------------------ */
/* Drawing helpers                                                          */

static void bar(int y, const char *text, int fg, int bg)
{
    int cols = video_text_cols();
    video_text_fill(0, y, cols, ' ', fg, bg);
    video_text(1, y, text, fg, bg);
}

/* Prints text from row y, wrapped at the screen width; returns the next row. */
static int wrap(int y, const char *text, int fg)
{
    int cols = video_text_cols() - 2, rows = video_text_rows();
    char line[96];
    while (*text && y < rows - 1) {
        int n = 0;
        while (text[n] && text[n] != '\n' && n < cols && n < (int)sizeof line - 1)
            n++;
        memcpy(line, text, (size_t)n);
        line[n] = 0;
        video_text(1, y++, line, fg, C_BLACK);
        text += n;
        if (*text == '\n')
            text++;
    }
    return y;
}

void ui_message(const char *title, const char *msg, bool show_log)
{
    video_text_mode();
    bar(0, title, C_WHITE, C_RED);
    int y = wrap(2, msg, C_YELLOW) + 1;
    if (show_log) {
        static char tail[1600];
        pal_log_tail(tail, sizeof tail, video_text_rows() - y - 3);
        wrap(y, tail, C_GRAY);
    }
    bar(video_text_rows() - 1, "Press Enter / A to continue", C_BLACK, C_GRAY);
    ui_keys_reset();
    for (;;) {
        int k = ui_key_wait();
        if (k == UI_OK || k == UI_BACK || k == UI_MENU)
            break;
    }
}

/* ------------------------------------------------------------------------ */
/* Screen sizes                                                             */

/* The sizes offered for a game: its own size first when it is not one of
 * the supported ones (shown crop-to-view), then the supported sizes. */
static int screen_choices(const GameSettings *s, int out[][2])
{
    int n = 0;
    if (s->orig_w && !settings_supported(s->orig_w, s->orig_h)) {
        out[n][0] = s->orig_w;
        out[n][1] = s->orig_h;
        n++;
    }
    for (int i = 0; i < nj_num_screen_sizes && n < 16; i++) {
        out[n][0] = nj_screen_sizes[i][0];
        out[n][1] = nj_screen_sizes[i][1];
        n++;
    }
    return n;
}

static int screen_choice_index(const GameSettings *s, int choices[][2], int n)
{
    for (int i = 0; i < n; i++)
        if (choices[i][0] == s->w && choices[i][1] == s->h)
            return i;
    return -1;
}

/* "176x208", "176x204 original", "176x204 original in 176x208" */
static void screen_label(const GameSettings *s, char *out, size_t size)
{
    bool orig = s->orig_w && s->w == s->orig_w && s->h == s->orig_h;
    if (s->view_w != s->w || s->view_h != s->h)
        snprintf(out, size, "%dx%d%s in %dx%d", s->w, s->h, orig ? " original" : "",
                 s->view_w, s->view_h);
    else
        snprintf(out, size, "%dx%d%s", s->w, s->h, orig ? " original" : "");
}

/* ------------------------------------------------------------------------ */
/* Launcher                                                                 */

#define MAX_ENTRIES 512

typedef struct {
    char *name;
    bool  dir;
} Entry;

static Entry entries[MAX_ENTRIES];
static int   n_entries;
static char  cur_dir[256] = NJ_HOME_DIR;
static char  last_pick[320];   /* the game started last, selected again */

static bool has_ext(const char *name, const char *ext)
{
    size_t a = strlen(name), b = strlen(ext);
    return a > b && strcasecmp(name + a - b, ext) == 0;
}

static int entry_cmp(const void *pa, const void *pb)
{
    const Entry *a = pa, *b = pb;
    if (a->dir != b->dir)
        return a->dir ? -1 : 1;
    if (strcmp(a->name, "..") == 0)
        return -1;
    if (strcmp(b->name, "..") == 0)
        return 1;
    return strcasecmp(a->name, b->name);
}

static void free_entries(void)
{
    for (int i = 0; i < n_entries; i++)
        free(entries[i].name);
    n_entries = 0;
}

static void add_entry(const char *name, bool dir)
{
    if (n_entries == MAX_ENTRIES)
        return;
    char *copy = strdup(name);
    if (!copy)
        return;
    entries[n_entries].name = copy;
    entries[n_entries].dir = dir;
    n_entries++;
}

static void scan_dir(void)
{
    free_entries();
    if (strcmp(cur_dir, "/") != 0)
        add_entry("..", true);
    DIR     d;
    FILINFO fi;
    if (f_opendir(&d, cur_dir) != FR_OK)
        return;
    while (f_readdir(&d, &fi) == FR_OK && fi.fname[0]) {
        if (fi.fattrib & (AM_HID | AM_SYS))
            continue;
        if (fi.fname[0] == '.')
            continue;
        bool dir = (fi.fattrib & AM_DIR) != 0;
        if (dir || has_ext(fi.fname, ".jar") || has_ext(fi.fname, ".jad"))
            add_entry(fi.fname, dir);
    }
    f_closedir(&d);
    qsort(entries, (size_t)n_entries, sizeof(Entry), entry_cmp);
}

/* dir + "/" + name; an empty string when it does not fit */
static void join(char *out, size_t size, const char *dir, const char *name)
{
    size_t a = strcmp(dir, "/") == 0 ? 0 : strlen(dir), b = strlen(name);
    if (a + b + 2 > size) {
        out[0] = 0;
        return;
    }
    memcpy(out, dir, a);
    out[a] = '/';
    memcpy(out + a + 1, name, b + 1);
}

static void draw_launcher(int sel, int top, int list_rows)
{
    int  cols = video_text_cols(), rows = video_text_rows();
    char line[128];
    snprintf(line, sizeof line, "pico-j2me %s - J2ME for RP2350%s", NJ_VERSION,
             audio_gm_ready() ? "  [MIDI: GM bank]" : "");
    bar(0, line, C_WHITE, C_BLUE);
    snprintf(line, sizeof line, "%.*s", cols - 2, cur_dir);
    bar(1, line, C_LCYAN, C_BLACK);
    for (int i = 0; i < list_rows; i++) {
        int e = top + i, y = 2 + i;
        video_text_fill(0, y, cols, ' ', C_GRAY, C_BLACK);
        if (e >= n_entries)
            continue;
        bool cur = e == sel;
        int  fg = entries[e].dir ? C_YELLOW : C_WHITE;
        int  bg = cur ? C_CYAN : C_BLACK;
        if (cur)
            fg = C_BLACK;
        if (entries[e].dir)
            snprintf(line, sizeof line, " [%.*s]", cols - 5, entries[e].name);
        else
            snprintf(line, sizeof line, " %.*s", cols - 2, entries[e].name);
        video_text_fill(0, y, cols, ' ', fg, bg);
        video_text(0, y, line, fg, bg);
    }
    /* status: the screen size of the selected game */
    if (n_entries == 0) {
        snprintf(line, sizeof line, "No .jar / .jad files here. Put games in %s", NJ_HOME_DIR);
    } else if (!entries[sel].dir) {
        char path[320], size[48];
        GameSettings s;
        join(path, sizeof path, cur_dir, entries[sel].name);
        settings_load(path, &s);
        screen_label(&s, size, sizeof size);
        snprintf(line, sizeof line, "Screen %s  (Left/Right)", size);
    } else {
        line[0] = 0;
    }
    bar(rows - 2, line, C_LGREEN, C_BLACK);
    bar(rows - 1, "Enter/A: start  Esc/B: up  Left/Right: screen size", C_BLACK, C_GRAY);
}

static void change_size(int sel, int dir)
{
    char path[320];
    GameSettings s;
    join(path, sizeof path, cur_dir, entries[sel].name);
    settings_load(path, &s);
    int choices[16][2] = {{0}};
    int n = screen_choices(&s, choices);
    int i = screen_choice_index(&s, choices, n);
    i = i < 0 ? 0 : (i + dir + n) % n;
    settings_set_screen(&s, choices[i][0], choices[i][1]);
    settings_save(path, &s);
}

char *ui_launcher(void)
{
    video_text_mode();
    if (!nj_sd_ok) {
        ui_message("No SD card", "The SD card could not be read.\nInsert a FAT32/exFAT card "
                   "with games in " NJ_HOME_DIR " and press a key to restart.", false);
        nj_safe_reset();
    }
    FILINFO fi;
    if (f_stat(cur_dir, &fi) != FR_OK && !sys_mkdirs(cur_dir))
        strcpy(cur_dir, "/");

    int rows = video_text_rows(), list_rows = rows - 4;
    scan_dir();
    int sel = 0, top = 0;
    for (int i = 0; i < n_entries && last_pick[0]; i++) {
        char path[320];
        join(path, sizeof path, cur_dir, entries[i].name);
        if (strcmp(path, last_pick) == 0)
            sel = i;
    }
    ui_keys_reset();
    for (;;) {
        if (sel >= n_entries)
            sel = n_entries ? n_entries - 1 : 0;
        if (sel < top)
            top = sel;
        if (sel >= top + list_rows)
            top = sel - list_rows + 1;
        draw_launcher(sel, top, list_rows);
        int k = ui_key_wait();
        switch (k) {
        case UI_UP:        sel = sel > 0 ? sel - 1 : (n_entries ? n_entries - 1 : 0); break;
        case UI_DOWN:      sel = sel + 1 < n_entries ? sel + 1 : 0; break;
        case UI_PAGE_UP:   sel = sel > list_rows ? sel - list_rows : 0; break;
        case UI_PAGE_DOWN: sel += list_rows; break;
        case UI_HOME:      sel = 0; break;
        case UI_END:       sel = n_entries - 1; break;
        case UI_LEFT:
        case UI_RIGHT:
            if (n_entries && !entries[sel].dir)
                change_size(sel, k == UI_RIGHT ? 1 : -1);
            break;
        case UI_BACK:
            if (strcmp(cur_dir, "/") != 0) {
                char *slash = strrchr(cur_dir, '/');
                char  prev[256];
                snprintf(prev, sizeof prev, "%s", slash + 1);
                if (slash == cur_dir)
                    cur_dir[1] = 0;
                else
                    *slash = 0;
                scan_dir();
                sel = top = 0;
                for (int i = 0; i < n_entries; i++)
                    if (entries[i].dir && strcmp(entries[i].name, prev) == 0)
                        sel = i;
            }
            break;
        case UI_OK:
            if (!n_entries)
                break;
            if (entries[sel].dir) {
                if (strcmp(entries[sel].name, "..") == 0) {
                    char *slash = strrchr(cur_dir, '/');
                    if (slash == cur_dir)
                        cur_dir[1] = 0;
                    else if (slash)
                        *slash = 0;
                } else if (strlen(cur_dir) + strlen(entries[sel].name) + 2 < sizeof cur_dir) {
                    char path[sizeof cur_dir];
                    join(path, sizeof path, cur_dir, entries[sel].name);
                    memcpy(cur_dir, path, sizeof cur_dir);
                }
                scan_dir();
                sel = top = 0;
            } else {
                char path[320];
                join(path, sizeof path, cur_dir, entries[sel].name);
                strcpy(last_pick, path);   /* both are 320 bytes */
                free_entries();
                return strdup(path);
            }
            break;
        default:
            break;
        }
    }
}

/* ------------------------------------------------------------------------ */
/* Emulator menu                                                            */

enum { M_RESUME, M_RESTART, M_SCREEN, M_VOLUME, M_EXIT, M_REBOOT, M_COUNT };

int ui_game_menu(GameSettings *s, const char *jar)
{
    video_text_mode();
    ui_keys_reset();
    int  sel = 0;
    int  choices[16][2] = {{0}};
    int  n_choices = screen_choices(s, choices);
    int  size_i = screen_choice_index(s, choices, n_choices);
    GameSettings shown = *s;   /* the screen chosen in the menu */
    int new_w = s->w, new_h = s->h;
    int result = EXIT_NONE;
    bool done = false;
    while (!done) {
        int  cols = video_text_cols();
        char line[96], name[64];
        settings_game_name(jar ? jar : "", name, sizeof name);
        snprintf(line, sizeof line, "Paused: %s", name);
        bar(0, line, C_WHITE, C_BLUE);
        const char *labels[M_COUNT];
        char screen[80], volume[40], size[48];
        settings_set_screen(&shown, new_w, new_h);
        screen_label(&shown, size, sizeof size);
        snprintf(screen, sizeof screen, "Screen: %s%s", size,
                 new_w != s->w || new_h != s->h ? " (restarts)" : "");
        snprintf(volume, sizeof volume, "Volume: %d%%", s->volume);
        labels[M_RESUME] = "Resume";
        labels[M_RESTART] = "Restart the game";
        labels[M_SCREEN] = screen;
        labels[M_VOLUME] = volume;
        labels[M_EXIT] = "Exit to the game list";
        labels[M_REBOOT] = "Reboot";
        for (int i = 0; i < M_COUNT; i++) {
            int y = 3 + i * 2;
            bool cur = i == sel;
            video_text_fill(0, y, cols, ' ', C_GRAY, C_BLACK);
            video_text_fill(2, y, cols - 4, ' ', cur ? C_BLACK : C_WHITE, cur ? C_CYAN : C_BLACK);
            video_text(4, y, labels[i], cur ? C_BLACK : C_WHITE, cur ? C_CYAN : C_BLACK);
        }
        bar(video_text_rows() - 1, "Enter/A: choose  Left/Right: change  Esc/B: resume",
            C_BLACK, C_GRAY);
        int k = ui_key_wait();
        switch (k) {
        case UI_UP:   sel = (sel + M_COUNT - 1) % M_COUNT; break;
        case UI_DOWN: sel = (sel + 1) % M_COUNT; break;
        case UI_BACK:
        case UI_MENU:
            done = true;
            break;
        case UI_LEFT:
        case UI_RIGHT: {
            int d = k == UI_RIGHT ? 1 : -1;
            if (sel == M_SCREEN) {
                size_i = size_i < 0 ? 0 : (size_i + d + n_choices) % n_choices;
                new_w = choices[size_i][0];
                new_h = choices[size_i][1];
            } else if (sel == M_VOLUME) {
                s->volume += d * 10;
                s->volume = s->volume < 0 ? 0 : s->volume > 100 ? 100 : s->volume;
            }
            break;
        }
        case UI_OK:
            switch (sel) {
            case M_RESUME:  done = true; break;
            case M_RESTART: result = EXIT_RESTART_GAME; done = true; break;
            case M_EXIT:    result = EXIT_CHANGE_GAME; done = true; break;
            case M_REBOOT:  nj_safe_reset();
            case M_SCREEN:
                if (new_w != s->w || new_h != s->h) {
                    result = EXIT_RESTART_GAME;
                    done = true;
                }
                break;
            default: break;
            }
            break;
        default:
            break;
        }
    }
    if (new_w != s->w || new_h != s->h) {
        settings_set_screen(s, new_w, new_h);
        if (result == EXIT_NONE)
            result = EXIT_RESTART_GAME;
    }
    settings_save(jar, s);
    ui_keys_reset();
    return result;
}
