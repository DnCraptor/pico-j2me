/*
 * gmbank.c - loads the General MIDI wavetable bank (gm_bank.bin) from the
 * SD card into PSRAM at boot, for src/audio/gmsynth.c.
 *
 * The bank is looked for in /.config/pico-j2me/, in the root of the card
 * and in pico-speccy's /.config/pico-speccy/ (the same file works for both:
 * both mix at 31250 Hz).  It is packed on a PC from a GM.DLS file
 * (tools/dls_pack.py) or a GUS / freepats patch set (tools/gus_pack.py).
 *
 * It takes the top of the PSRAM for good; the games' native heap shrinks by
 * its size.  Without PSRAM, without a bank, or with a bank that does not
 * fit, MIDI plays on the built-in waveform synthesizer as before.
 */
#include "rp2350.h"
#include "../../src/audio/gmsynth.h"

#include <stdio.h>
#include <stdlib.h>

#include "ff.h"

/* What the games keep at least: their jar, classes and a Java heap. */
#define MIN_GAME_PSRAM (3u << 20)

static const char *const bank_paths[] = {
    NJ_CONFIG_DIR "/gm_bank.bin",
    "/gm_bank.bin",
    "/.config/pico-speccy/gm_bank.bin",
};

static void report(const char *path, const char *msg)
{
    char text[200];
    snprintf(text, sizeof text, "%s:\n%s\n\nMIDI plays on the built-in synthesizer.", path, msg);
    printf("gm_bank: %s: %s\n", path, msg);
    ui_message("General MIDI bank not used", text, false);
}

void audio_gm_load(void)
{
    if (!nj_sd_ok || !sys_psram())
        return;
    const char *path = NULL;
    FILINFO     fi;
    for (size_t i = 0; i < sizeof bank_paths / sizeof bank_paths[0]; i++)
        if (f_stat(bank_paths[i], &fi) == FR_OK && !(fi.fattrib & AM_DIR)) {
            path = bank_paths[i];
            break;
        }
    if (!path)
        return;

    uint32_t size = (uint32_t)fi.fsize;
    if (fi.fsize > 0xFFFFFFFFu || sys_psram_size() < MIN_GAME_PSRAM ||
        size > sys_psram_size() - MIN_GAME_PSRAM) {
        char msg[96];
        snprintf(msg, sizeof msg, "%u KB does not fit: %u KB of PSRAM, %u KB kept for the games",
                 (unsigned)(fi.fsize >> 10), (unsigned)(sys_psram_size() >> 10),
                 (unsigned)(MIN_GAME_PSRAM >> 10));
        report(path, msg);
        return;
    }
    void *state = malloc(gmsynth_state_bytes());   /* SRAM: read per sample */
    uint8_t *blob = sys_mem_reserve_top(size);
    if (!state || !blob) {
        free(state);
        if (blob)
            sys_mem_unreserve_top(size);
        report(path, "out of memory");
        return;
    }

    video_text_mode();
    char line[80];
    snprintf(line, sizeof line, "Loading %s (%u KB)...", path, (unsigned)(size >> 10));
    video_text(1, 1, line, 15, 0);

    FIL  f;
    UINT got = 0;
    bool ok = f_open(&f, path, FA_READ) == FR_OK;
    if (ok) {
        ok = f_read(&f, blob, size, &got) == FR_OK && got == size;
        f_close(&f);
    }
    const char *why = "cannot be read";
    if (ok)
        ok = audio_gm_bind(blob, size, state, &why);
    if (!ok) {
        sys_mem_unreserve_top(size);
        free(state);
        report(path, why);
        return;
    }
    printf("gm_bank: %s, %u KB in PSRAM\n", path, (unsigned)(size >> 10));
}
