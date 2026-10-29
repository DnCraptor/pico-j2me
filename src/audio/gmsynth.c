/*
 * gmsynth.c - General MIDI wavetable synthesis; see gmsynth.h.
 *
 * Hosts xrip's embeded-midi-synth engine (external/embeded-midi-synth,
 * licensing in its NOTICE) in this one translation unit.  The engine keeps
 * one global state (g_voices, g_channels, g_active_mask, g_next_age); each
 * synthesizer slot has its own copy, swapped in around every call.  The
 * bank itself (g_bank and the decode tables) is shared.
 */
#include "gmsynth.h"
#include "synth.h"

#include <string.h>

#include "../../external/embeded-midi-synth/gm_bank.h"

#define INLINE           static inline
#define SOUND_FREQUENCY  GMSYNTH_RATE
#define WT_MAX_VOICES    24       /* per slot */
#define WT_NO_WAVE_CACHE 1        /* the bank is directly addressable (PSRAM) */
#include "../../external/embeded-midi-synth/wavetable.c.inl"

typedef struct {
    wt_voice_t  *voices;
    wt_channel_t channels[WT_MIDI_CHANNELS];
    uint32_t     active_mask;
    uint32_t     next_age;
} Context;

static Context ctx[SYNTH_SLOTS];
static bool    bound;

size_t gmsynth_state_bytes(void)
{
    return (size_t)SYNTH_SLOTS * WT_MAX_VOICES * sizeof(wt_voice_t);
}

static void load(int slot)
{
    Context *c = &ctx[slot];
    g_voices = c->voices;
    memcpy(g_channels, c->channels, sizeof g_channels);
    g_active_mask = c->active_mask;
    g_next_age = c->next_age;
}

static void save(int slot)
{
    Context *c = &ctx[slot];
    memcpy(c->channels, g_channels, sizeof g_channels);
    c->active_mask = g_active_mask;
    c->next_age = g_next_age;
}

bool gmsynth_bind(const void *blob, uint32_t size, void *state, const char **why)
{
    const gm_bank_header_t *h = blob;
    gm_bank_view_t view;
    bound = false;
    if (size < sizeof *h || !gm_bank_view(blob, &view)) {
        *why = "not a gm_bank.bin (v5)";
        return false;
    }
    if (h->output_rate != GMSYNTH_RATE) {
        *why = "packed for another sample rate (needs 31250 Hz)";
        return false;
    }
    if (h->off_pcm > size || h->pcm_samples > size - h->off_pcm ||
        h->off_instruments > size || h->off_regions > size || h->off_waves > size) {
        *why = "truncated";
        return false;
    }
    wt_voice_t *v = state;
    for (int s = 0; s < SYNTH_SLOTS; s++)
        ctx[s].voices = v + s * WT_MAX_VOICES;
    /* wt_set_bank() also resets the loaded context: do it for each slot */
    load(0);
    wt_set_bank(blob);
    save(0);
    for (int s = 1; s < SYNTH_SLOTS; s++) {
        load(s);
        wt_engine_reset();
        save(s);
    }
    bound = true;
    return true;
}

bool gmsynth_ready(void)
{
    return bound;
}

void gmsynth_reset(int slot)
{
    if (!bound)
        return;
    load(slot);
    wt_engine_reset();
    save(slot);
}

void gmsynth_message(int slot, uint8_t status, uint8_t d1, uint8_t d2)
{
    if (!bound)
        return;
    load(slot);
    midi_command_t m = {status, d1, d2, 0};
    parse_midi(&m);
    save(slot);
}

void gmsynth_notes_off(int slot, bool immediately)
{
    if (!bound)
        return;
    load(slot);
    for (uint8_t ch = 0; ch < WT_MIDI_CHANNELS; ch++) {
        /* CC 120 all sound off (cut), or sustain off + CC 123 all notes off */
        if (immediately) {
            midi_command_t m = {(uint8_t)(0xB0 | ch), 0x78, 0, 0};
            parse_midi(&m);
        } else {
            midi_command_t s = {(uint8_t)(0xB0 | ch), 0x40, 0, 0};
            midi_command_t m = {(uint8_t)(0xB0 | ch), 0x7B, 0, 0};
            parse_midi(&s);
            parse_midi(&m);
        }
    }
    save(slot);
}

bool gmsynth_sounding(int slot)
{
    return bound && ctx[slot].active_mask != 0;
}

void gmsynth_mix(int32_t *acc, int n, const int gain[])
{
    if (!bound)
        return;
    for (int s = 0; s < SYNTH_SLOTS; s++) {
        if (!ctx[s].active_mask)
            continue;
        load(s);
        const int g = gain[s];
        for (int f = 0; f < n; f++) {
            int16_t l, r;
            midi_sample_stereo(&l, &r);
            acc[2 * f] += l * g / 100;
            acc[2 * f + 1] += r * g / 100;
        }
        save(s);
    }
}
