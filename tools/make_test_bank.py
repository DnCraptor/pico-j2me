#!/usr/bin/env python3
# Writes a tiny synthetic gm_bank.bin (GMWB v5, 31250 Hz) for testing the
# wavetable path without a real sound bank: one melodic instrument (bank 0,
# program 0 - every other program falls back to it) playing a looped sine,
# and a drum kit playing a short decaying noise burst.
#
#   python3 tools/make_test_bank.py out.bin
import math
import random
import struct
import sys

sys.dont_write_bytecode = True
sys.path.insert(0, __import__("os").path.dirname(__file__))
from dls_pack import (HEADER_FMT, INSTR_FMT, WAVE_FMT, GM_BANK_MAGIC, GM_BANK_VERSION,
                      GM_RGN_LOOPED, DLS_DRUM_BANK, gm_linear2ulaw, pack_region, new_region_fields)

RATE = 31250


def main(argv):
    if len(argv) != 2:
        sys.stderr.write("usage: make_test_bank.py <out.bin>\n")
        return 2
    # wave 0: one sine cycle of 64 samples at 31250 Hz = 488.28 Hz, root key
    # chosen so the note plays at its MIDI pitch closely enough for a test
    cycle = 64
    sine = [int(20000 * math.sin(2 * math.pi * i / cycle)) for i in range(cycle * 4)]
    random.seed(1)
    noise = [int(random.uniform(-20000, 20000) * math.exp(-i / 1500.0)) for i in range(6000)]
    pcm = bytes(gm_linear2ulaw(s) for s in sine) + bytes(gm_linear2ulaw(s) for s in noise)
    waves = [(0, len(sine), 65536), (len(sine), len(noise), 65536)]

    def region(wave, looped, root, decay_s, sustain):
        R = new_region_fields()
        R["gain_q16"] = 65536
        R["attack_step_q16"] = 65536            # instant attack
        R["decay_coef_q16"] = int(65536 * 10.0 ** (-4.8 / (decay_s * RATE)))
        R["release_coef_q16"] = int(65536 * 10.0 ** (-4.8 / (0.2 * RATE)))
        R["sustain_q16"] = sustain
        R["wave_index"] = wave
        R["key_low"], R["key_high"], R["vel_low"], R["vel_high"] = 0, 127, 0, 127
        R["root_key"] = root
        if looped:
            R["flags"] |= GM_RGN_LOOPED
            R["loop_start"], R["loop_length"] = 0, len(sine)
        return pack_region(R)

    # 488.28 Hz is MIDI 71.0 - 0.17 semitones; root 71 is close enough
    regions = region(0, True, 71, 1.0, 40000) + region(1, False, 60, 0.3, 0)
    instruments = [(0, 0, 0, 1), (DLS_DRUM_BANK, 1, 0, 1)]
    off_ins = 44
    off_rgn = off_ins + len(instruments) * 12
    off_wav = off_rgn + 2 * 80
    off_pcm = off_wav + len(waves) * 12
    with open(argv[1], "wb") as f:
        f.write(struct.pack(HEADER_FMT, GM_BANK_MAGIC, GM_BANK_VERSION, RATE, len(instruments),
                            2, len(waves), off_ins, off_rgn, off_wav, off_pcm, len(pcm)))
        for it in instruments:
            f.write(struct.pack(INSTR_FMT, *it))
        f.write(regions)
        for w in waves:
            f.write(struct.pack(WAVE_FMT, *w))
        f.write(pcm)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
