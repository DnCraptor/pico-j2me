/*
 * audio_rp2350.c - sound: the synthesizer (src/audio/synth.c) drives a
 * software model of 16 DS-style sound channels (looping single-cycle
 * waveforms, LFSR noise, 16-bit PCM), mixed on core 1 and sent to I2S or
 * PWM by the Murmulator audio driver.
 *
 * The pal_audio_* calls come from core 0 (the VM); the mixer runs on core
 * 1.  A spin lock keeps the synthesizer state consistent between them.
 *
 * With a General MIDI bank (gm_bank.bin, see audio_gm_load) the songs are
 * rendered by the wavetable engine (src/audio/gmsynth.c) and added here.
 */
#include "rp2350.h"

#include <string.h>

#include "pico/stdlib.h"
#include "hardware/sync.h"
#include "hardware/dma.h"

#include "audio.h"
#include "../../src/audio/gmsynth.h"

#define MIX_RATE    GMSYNTH_RATE                  /* 31250: the gm_bank.bin rate */
#define TICK_MS     4
#define TICK_FRAMES (MIX_RATE * TICK_MS / 1000)   /* 125 */
#define BLOCK       (TICK_FRAMES * 2)             /* frames per DMA block (8 ms) */

enum { CH_OFF, CH_WAVE, CH_NOISE, CH_PCM };

typedef struct {
    int            kind;
    const int8_t  *wave;
    const int16_t *pcm;
    uint32_t       len;       /* samples */
    uint32_t       pos;       /* 16.16 fixed point for waves, integer part = sample */
    uint32_t       frac;      /* PCM / noise: fractional position, 0.32 */
    uint32_t       step;      /* 16.16 */
    int            vol, pan;
    uint16_t       lfsr;
    int            noise_out;
} Channel;

static Channel       channels[SYNTH_CHANNELS];
static spin_lock_t  *lock;
static i2s_config_t  i2s;
/* DMA never needs malloc, even on a board without PSRAM. */
static uint32_t      hw_dma_buffer[BLOCK];
static int16_t       block[2][BLOCK * 2];
static int           dma_block = -1;
static int           prepared_block = -1;
static volatile bool paused = true;
static volatile int  master = 80;

static uint32_t step_for(uint32_t rate)
{
    return (uint32_t)(((uint64_t)rate << 16) / MIX_RATE);
}

static void hw_wave(int ch, const int8_t *data, int len, uint32_t rate, int vol, int pan)
{
    Channel *c = &channels[ch];
    c->kind = CH_WAVE;
    c->wave = data;
    c->len = (uint32_t)len;
    c->pos = 0;
    c->step = step_for(rate);
    c->vol = vol;
    c->pan = pan;
}

static void hw_noise(int ch, uint32_t rate, int vol, int pan)
{
    Channel *c = &channels[ch];
    c->kind = CH_NOISE;
    c->pos = 0;
    c->step = step_for(rate);
    c->vol = vol;
    c->pan = pan;
    c->frac = 0;
    c->lfsr = 0x7FFF;
    c->noise_out = 0;
}

static void hw_pcm16(int ch, const int16_t *data, uint32_t samples, uint32_t rate, int vol, int pan)
{
    Channel *c = &channels[ch];
    c->kind = CH_PCM;
    c->pcm = data;
    c->len = samples;
    c->pos = 0;
    c->frac = 0;
    c->step = step_for(rate);
    c->vol = vol;
    c->pan = pan;
}

static void hw_volume(int ch, int vol, int pan)
{
    channels[ch].vol = vol;
    channels[ch].pan = pan;
}

static void hw_rate(int ch, uint32_t rate)
{
    channels[ch].step = step_for(rate);
}

static void hw_stop(int ch)
{
    channels[ch].kind = CH_OFF;
}

static bool hw_active(int ch)
{
    return channels[ch].kind != CH_OFF;
}

static const SynthHw rp_hw = {
    hw_wave, hw_noise, hw_pcm16, hw_volume, hw_rate, hw_stop, hw_active
};

/* Mixes n (at most TICK_FRAMES) stereo frames into out, on core 1.
 * Waves keep a 16.16 position (a wave is 32 samples); PCM and noise keep
 * an integer sample index in pos and the 16-bit fraction in frac. */
static int32_t acc[TICK_FRAMES * 2];
static int32_t gm_acc[TICK_FRAMES * 2];

static void __not_in_flash_func(mix)(int16_t *out, int n)
{
    memset(acc, 0, sizeof(int32_t) * 2 * (size_t)n);
    for (int i = 0; i < SYNTH_CHANNELS; i++) {
        Channel *c = &channels[i];
        if (c->kind == CH_OFF || c->vol == 0)
            continue;
        int vl = c->vol * (127 - c->pan), vr = c->vol * c->pan;   /* 0..16129 */
        switch (c->kind) {
        case CH_WAVE: {
            const uint32_t end = c->len << 16;
            uint32_t pos = c->pos;
            for (int f = 0; f < n; f++) {
                int s = c->wave[pos >> 16] * 256;
                acc[2 * f] += s * vl >> 14;
                acc[2 * f + 1] += s * vr >> 14;
                pos += c->step;
                while (pos >= end)
                    pos -= end;
            }
            c->pos = pos;
            break;
        }
        case CH_PCM: {
            uint32_t idx = c->pos, frac = c->frac;
            for (int f = 0; f < n; f++) {
                if (idx >= c->len) {
                    c->kind = CH_OFF;
                    break;
                }
                int s = c->pcm[idx];
                acc[2 * f] += s * vl >> 14;
                acc[2 * f + 1] += s * vr >> 14;
                frac += c->step;
                idx += frac >> 16;
                frac &= 0xFFFF;
            }
            c->pos = idx;
            c->frac = frac;
            break;
        }
        case CH_NOISE: {
            /* The DS's 15-bit LFSR noise, clocked at the channel rate. */
            uint32_t frac = c->frac;
            int      s = c->noise_out;
            for (int f = 0; f < n; f++) {
                frac += c->step;
                for (uint32_t k = frac >> 16; k; k--) {
                    int bit = c->lfsr & 1;
                    c->lfsr >>= 1;
                    if (bit)
                        c->lfsr ^= 0x6000;
                    s = bit ? -0x7FFF : 0x7FFF;
                }
                frac &= 0xFFFF;
                acc[2 * f] += s * vl >> 14;
                acc[2 * f + 1] += s * vr >> 14;
            }
            c->frac = frac;
            c->noise_out = s;
            break;
        }
        }
    }
    int m = master;
    if (gmsynth_ready()) {
        int gain[SYNTH_SLOTS];
        synth_gains(gain);
        for (int f = 0; f < 2 * n; f++)
            gm_acc[f] = 0;
        gmsynth_mix(gm_acc, n, gain);
    }
    for (int f = 0; f < 2 * n; f++) {
        /* Same headroom as the DS mixer: 16 full channels would clip;
         * the wavetable output is already scaled. */
        int32_t v = (acc[f] / 4 + (gmsynth_ready() ? gm_acc[f] : 0)) * m / 100;
        out[f] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
    }
}

void audio_core1_init(void)
{
    lock = spin_lock_init(spin_lock_claim_unused(true));
    synth_init(&rp_hw);
    i2s = i2s_get_default_config();
    i2s.sample_freq = MIX_RATE;
    i2s.dma_trans_count = BLOCK;
    i2s.dma_buf = (uint16_t *)hw_dma_buffer;
    i2s_volume(&i2s, 0);
    i2s_init(&i2s);
}

/* Core 1 is also responsible for the HDMI scanline IRQ.  Never mask its
 * interrupts for the duration of audio synthesis: a missed scanline IRQ
 * corrupts the HDMI timing.  Only core 0 uses this synthesizer lock from
 * task context; the video IRQ does not touch the synthesizer. */
static void __not_in_flash_func(audio_render_block)(int which)
{
    if (paused) {
        memset(block[which], 0, sizeof block[which]);
    } else {
        spin_lock_unsafe_blocking(lock);
        for (int t = 0; t < BLOCK / TICK_FRAMES; t++) {
            synth_tick(TICK_MS);
            mix(block[which] + 2 * t * TICK_FRAMES, TICK_FRAMES);
        }
        spin_unlock_unsafe(lock);
    }
}

/* Prepare one block while the preceding one is still being transmitted.
 * The driver owns a separate DMA buffer, so the prepared samples remain
 * untouched until i2s_dma_write copies them after DMA completion. */
void __not_in_flash_func(audio_core1_poll)(void)
{
    if (dma_block < 0) {
        audio_render_block(0);
        i2s_dma_write(&i2s, block[0]);
        dma_block = 0;
        return;
    }

    const int next = dma_block ^ 1;
    if (dma_channel_is_busy(i2s.dma_channel)) {
        if (prepared_block < 0) {
            audio_render_block(next);
            prepared_block = next;
        }
        return;
    }

    /* Normally ready already; render here only after an actual overrun. */
    if (prepared_block != next)
        audio_render_block(next);
    i2s_dma_write(&i2s, block[next]);
    dma_block = next;
    prepared_block = -1;
}

void audio_pause(bool p)
{
    paused = p;
}

void audio_master(int volume)
{
    master = volume < 0 ? 0 : volume > 100 ? 100 : volume;
}

void audio_reset(void)
{
    uint32_t save = spin_lock_blocking(lock);
    for (int slot = 0; slot < SYNTH_SLOTS; slot++)
        synth_stop(slot);
    for (int ch = 0; ch < SYNTH_CHANNELS; ch++)
        channels[ch].kind = CH_OFF;
    spin_unlock(lock, save);
}

/* Binds a General MIDI bank under the lock: core 1 may be mixing. */
bool audio_gm_bind(const void *blob, uint32_t size, void *state, const char **why)
{
    spin_lock_unsafe_blocking(lock);
    bool ok = gmsynth_bind(blob, size, state, why);
    spin_unlock_unsafe(lock);
    return ok;
}

bool audio_gm_ready(void)
{
    return gmsynth_ready();
}

/* ------------------------------------------------------------------------ */
/* pal.h                                                                    */

bool pal_audio_available(void)
{
    return true;
}

void pal_audio_song(int slot, const NjaEvent *ev, uint32_t n, uint32_t start_ms, int volume)
{
    uint32_t save = spin_lock_blocking(lock);
    synth_song(slot, ev, n, start_ms, volume);
    spin_unlock(lock, save);
}

void pal_audio_pcm(int slot, const int16_t *pcm, uint32_t samples, uint32_t rate,
                   uint32_t start, int volume)
{
    uint32_t save = spin_lock_blocking(lock);
    synth_pcm(slot, pcm, samples, rate, start, volume);
    spin_unlock(lock, save);
}

void pal_audio_stop(int slot)
{
    uint32_t save = spin_lock_blocking(lock);
    synth_stop(slot);
    spin_unlock(lock, save);
}

void pal_audio_volume(int slot, int volume)
{
    uint32_t save = spin_lock_blocking(lock);
    synth_volume(slot, volume);
    spin_unlock(lock, save);
}
