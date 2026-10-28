/*
 * main.c - Nano Java for RP2350 boards (Murmulator and friends): start-up,
 * core 1, the launcher / game loop.
 *
 * Core 0: the launcher and the Java VM.  Core 1: the video driver, input
 * polling (PS/2, USB host, NES pad) and the sound mixer.
 */
#include "rp2350.h"
#include "psram.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/clocks.h"
#include "hardware/vreg.h"
#include "hardware/watchdog.h"
#include "hardware/sync.h"

#include "graphics.h"
#include "ff.h"

#include "../../src/vm/vm.h"
#include "../../src/util/zip.h"
#include "../../src/midp/midp.h"

#ifndef CPU_MHZ
#ifdef CPU_FREQ          /* a board header can ask for its clock (z0p2: 252) */
#define CPU_MHZ CPU_FREQ
#else
#define CPU_MHZ 378
#endif
#endif
#ifndef NJ_VERSION
#define NJ_VERSION "dev"
#endif

/* The class library, linked into flash (classlib.S). */
extern const uint8_t classlib_bin[];
extern const uint8_t classlib_bin_end[];

bool nj_sd_ok;
static FATFS fs;

/* ------------------------------------------------------------------------ */
/* Clocks and reset                                                         */

static void overclock(void)
{
    volatile uint32_t *qmi_m0_timing = (uint32_t *)0x400d000c;
    vreg_disable_voltage_limit();
    vreg_set_voltage(CPU_MHZ > 300 ? VREG_VOLTAGE_1_60 : VREG_VOLTAGE_1_30);
    sleep_ms(10);
    /* flash: slower QMI clock while clk_sys changes, then the final one */
    *qmi_m0_timing = 0x60007204;
    set_sys_clock_khz(CPU_MHZ * 1000, false);
    *qmi_m0_timing = 0x60007303;
}

/* Ctrl+Alt+Del, the menu's Reboot and fatal errors: the PSRAM must leave
 * QPI mode and clk_sys must drop before the watchdog reset, so the boot ROM
 * and the next firmware start at a safe clock. */
void __no_inline_not_in_flash_func(nj_safe_reset)(void)
{
    watchdog_enable(100, true);   /* failsafe if the clock change stalls */
    save_and_disable_interrupts();
    psram_prepare_reset();
    set_sys_clock_khz(150000, false);
    vreg_set_voltage(VREG_VOLTAGE_1_10);
    busy_wait_ms(10);
    watchdog_reboot(0, 0, 10);
    for (;;)
        tight_loop_contents();
}

/* ------------------------------------------------------------------------ */
/* Core 1                                                                   */

static volatile bool core1_ready;

static void __not_in_flash_func(core1_main)(void)
{
    multicore_lockout_victim_init();
    video_core1_init();
    input_core1_init();
    audio_core1_init();
    core1_ready = true;
    uint32_t frame = graphics_frame_count;
    for (;;) {
        if (frame != graphics_frame_count) {
            frame = graphics_frame_count;
            video_core1_vblank();
        }
        input_core1_poll();
        audio_core1_poll();
        tight_loop_contents();
    }
}

/* ------------------------------------------------------------------------ */
/* Games                                                                    */

static bool ends_with(const char *s, const char *suffix)
{
    size_t a = strlen(s), b = strlen(suffix);
    return a >= b && strcasecmp(s + a - b, suffix) == 0;
}

/* Reads a whole file into native memory (PSRAM). */
static uint8_t *read_file(const char *path, uint32_t *len)
{
    PalFile *f = pal_fopen(path, false);
    if (!f)
        return NULL;
    uint32_t size = pal_fsize(f);
    uint8_t *buf = pal_malloc((size_t)size + 1);
    if (buf && pal_fread(f, buf, size) != size) {
        pal_free(buf);
        buf = NULL;
    }
    pal_fclose(f);
    if (buf) {
        buf[size] = 0;
        *len = size;
    }
    return buf;
}

/* For a .jad: registers its properties and returns the jar it points to
 * (MIDlet-Jar-URL, looked up next to the .jad), or <name>.jar. */
static char *jar_for_jad(const char *jad_path)
{
    uint32_t len;
    char *jad = (char *)read_file(jad_path, &len);
    if (!jad)
        return NULL;
    res_set_jad((const uint8_t *)jad, len);
    char *jar = malloc(strlen(jad_path) + 256);
    if (!jar) {
        pal_free(jad);
        return NULL;
    }
    strcpy(jar, jad_path);
    char *base = strrchr(jar, '/');
    base = base ? base + 1 : jar;
    const char *url = strstr(jad, "MIDlet-Jar-URL:");
    bool found = false;
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
        if (n > 0 && n < 250) {
            memcpy(base, name, n);
            base[n] = 0;
            FILINFO fi;
            found = f_stat(jar, &fi) == FR_OK;
        }
    }
    if (!found) {
        strcpy(jar, jad_path);
        strcpy(jar + strlen(jar) - 4, ".jar");
    }
    pal_free(jad);
    return jar;
}

static int run_vm(const char *path, GameSettings *s)
{
    nj_exit_action = EXIT_NONE;
    sys_mem_reset();
    char *jar = (char *)path;
    if (ends_with(path, ".jad")) {
        jar = jar_for_jad(path);
        if (!jar) {
            ui_message("Cannot start the game", "The .jad file cannot be read.", false);
            return -1;
        }
    }

    /* The Java heap: half of the PSRAM (at most 4 MB), the rest for the
     * game's jar, class metadata, thread stacks and decoded sounds. */
    size_t heap;
    if (sys_psram()) {
        heap = sys_psram_size() / 2;
        if (heap > (4u << 20))
            heap = 4u << 20;
    } else {
        heap = 96u << 10;
    }
    VMOptions opt = {heap, 4096, 256, false, false, false};
    vm_init(&opt);
    midp_init();

    Zip *classlib = zip_open_mem(classlib_bin, (uint32_t)(classlib_bin_end - classlib_bin));
    if (!classlib)
        pal_fatal("The built-in class library is damaged.");
    res_set_system(classlib);

    /* With PSRAM the jar is read once into memory; without it, entries are
     * read from the card when needed. */
    uint8_t *jar_data = NULL;
    uint32_t jar_len = 0;
    Zip     *app = NULL;
    if (sys_psram()) {
        jar_data = read_file(jar, &jar_len);
        if (jar_data)
            app = zip_open_mem(jar_data, jar_len);
    } else {
        app = zip_open_file(jar);
    }
    if (!app) {
        vm_shutdown();
        midp_shutdown();
        if (jar != path)
            free(jar);
        ui_message("Cannot start the game", "The .jar file cannot be opened.", false);
        return -1;
    }
    res_add_app_zip(app);

    printf("Starting %s\n", jar);
    printf("Java heap: %u KB, PSRAM: %u KB\n", (unsigned)(heap >> 10),
           (unsigned)(sys_psram_size() >> 10));
    pal_session_start(s, jar);

    const char *boot_args[] = {"-midlet", ""};
    if (!vm_boot(boot_args, 2))
        pal_fatal("The VM failed to start.");
    vm_poll_hook = midp_poll;
    int code = vm_run();
    if (vm.uncaught)
        code = 1;
    audio_pause(true);
    vm_shutdown();
    midp_shutdown();
    audio_reset();
    if (jar != path)
        free(jar);
    return code;
}

int main(void)
{
    int stack_base;
    heap_set_stack_base(&stack_base);

    overclock();
    /* QMI CS1 set-up before core 1 starts: the probe briefly blocks XIP */
    psram_init();
    sys_mem_init();
    input_init();

    multicore_launch_core1(core1_main);
    while (!core1_ready)
        tight_loop_contents();

    nj_sd_ok = f_mount(&fs, "", 1) == FR_OK;
    printf("pico-j2me %s, %u MHz, PSRAM %u KB, SD %s\n", NJ_VERSION, CPU_MHZ,
           (unsigned)(psram_size() >> 10), nj_sd_ok ? "ok" : "missing");

    char *path = NULL;
    for (;;) {
        if (!path)
            path = ui_launcher();
        GameSettings s;
        settings_load(path, &s);
        int code = run_vm(path, &s);
        if (nj_exit_action == EXIT_RESTART_GAME)
            continue;
        if (nj_exit_action != EXIT_CHANGE_GAME && code > 0)
            ui_message("The MIDlet stopped", "The MIDlet ended with an error.", true);
        free(path);
        path = NULL;
    }
}
