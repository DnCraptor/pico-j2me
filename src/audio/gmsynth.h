/*
 * gmsynth.h - General MIDI wavetable synthesis with a real sound bank
 * (gm_bank.bin), using the fixed-point engine of xrip's embeded-midi-synth
 * (external/embeded-midi-synth, as in pico-speccy).
 *
 * When a bank is bound, the sequencer in synth.c sends the MIDI messages of
 * every song to this engine instead of its own waveform voices; PCM clips
 * still play on the synthesizer's channels.  Each of the SYNTH_SLOTS players
 * has its own engine state (16 MIDI channels and a voice pool), so two songs
 * playing at once do not take each other's instruments.
 *
 * The engine renders at the rate the bank was packed for (gm_bank.bin is
 * packed at 31250 Hz by default, tools/dls_pack.py / tools/gus_pack.py), so
 * the platform mixes at that rate.
 */
#ifndef NJ_GMSYNTH_H
#define NJ_GMSYNTH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define GMSYNTH_RATE 31250

/* Bytes of voice state the platform provides to gmsynth_bind() (fast RAM:
 * the render loop reads it for every sample). */
size_t gmsynth_state_bytes(void);

/* Checks the bank (magic, version, output rate) and binds it; the blob must
 * stay in place while bound.  state: gmsynth_state_bytes() bytes.  Returns
 * false (with a reason in *why) when the bank cannot be used. */
bool gmsynth_bind(const void *blob, uint32_t size, void *state, const char **why);
bool gmsynth_ready(void);

/* Called by synth.c (under the platform's synthesizer lock). */
void gmsynth_reset(int slot);                    /* a new song on the slot */
void gmsynth_message(int slot, uint8_t status, uint8_t d1, uint8_t d2);
void gmsynth_notes_off(int slot, bool immediately);
bool gmsynth_sounding(int slot);

/* Adds n stereo frames of every sounding slot to acc (interleaved L/R,
 * int16 scale), each slot scaled by gain[slot] (0..100). */
void gmsynth_mix(int32_t *acc, int n, const int gain[]);

#endif
