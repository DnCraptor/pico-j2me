# pico-j2me

**J2ME games and applications (MIDlets, CLDC 1.1 / MIDP 2.0) on RP2350-based
MURMULATOR systems and compatible boards with QSPI PSRAM.**

`pico-j2me` is a port of [Nano Java](https://github.com/mostazaniikkkk/Nano-Java)
(version 2.0, the from-scratch rewrite) from the Nintendo DS to the RP2350:
a small Java virtual machine in portable C, a class library written mostly
in Java and a platform layer - here for the Murmulator boards, with the
drivers of [pico-gameboy](https://github.com/DnCraptor/pico-gameboy).
No third-party Java code is used: no KVM, no Sun class libraries.

Current version: **0.1.1** (hot-fix).

## Supported boards

| Board | Firmware prefix | Video | Sound |
|---|---|---|---|
| MURMULATOR 1.x | `m1p2` | VGA, HDMI, TFT (ST7789 / ILI9341) | PWM, I2S |
| MURMULATOR 2.0 | `m2p2` | VGA, HDMI, TFT (ST7789 / ILI9341) | PWM, I2S |
| Olimex RP2040-PICO-PC with a Pico 2 | `PCp2` | HDMI (DVI connector) | PWM (3.5 mm jack) |
| Waveshare RP2350-PiZero | `z0p2` | mini-HDMI | I2S (an external PCM5102A module on GP10..12) |

Firmware files are named `<prefix>-j2me-<video>-<sound>-<cpu>MHz-<version>.uf2`.

**QSPI PSRAM is needed** for real games: the Java heap (half of the PSRAM,
at most 4 MB), the game's jar, the class metadata and decoded sounds live
there.  Without PSRAM the firmware still starts, with a 96 KB Java heap in
SRAM - enough for the smallest MIDlets only.

## Features

- Java VM: full bytecode set, green threads with monitors, `wait`/`notify`,
  exceptions with stack traces, weak references, mark & sweep GC
- CLDC 1.1: `java.lang`, `java.util` (`Timer`, `Calendar`...), `java.io`,
  floating point
- MIDP 2.0: `Canvas`, `GameCanvas`, `Graphics` (all primitives, sprite
  transforms, alpha, `drawRGB`), PNG images, fonts, commands, the high-level
  UI (`Form`, `List`, `TextBox`, `Alert`...), `lcdui.game` (`Sprite`,
  `TiledLayer`, `LayerManager`), record stores (`rms`), MMAPI sound: MIDI,
  WAV (PCM, IMA ADPCM, u-law, A-law), tone sequences
- Nokia UI API (`FullCanvas`, `DirectGraphics`, `DeviceControl`, `Sound`)
- JAR and JAD files
- SD-card game browser with a per-game phone screen size
- PS/2 and USB keyboards, NES/SNES gamepads

## Using it

1. Flash the UF2 for your board.
2. Put the games (`.jar`, optionally with their `.jad`) on the SD card in
   `/j2me/` (sub-folders are fine).
3. Pick a game in the list.  **Left / Right** changes the phone screen
   size of the selected game: 128x128, 128x160, 176x208, 176x220, 208x208,
   240x240, 240x320 or 320x240.

By default a game gets its original screen size when its `.jad` or manifest
declares one (`Nokia-MIDlet-Original-Display-Size`,
`Nokia-MIDlet-Target-Display-Size`, `MIDlet-Display-Size`), otherwise
240x320.  An original size that is none of the sizes above (176x204, say)
is shown **crop-to-view**: the game draws its own size and the nearest
supported size is the view, centred on it - what lies outside the view is
cut off, and where the view is larger than the game's picture the margin
stays black.  Such a size is offered first in the Left / Right list (and in
the emulator menu).  A chosen size is saved in the game's `.ini` only when
it differs from the default.

The picture is scaled to the 640x480 screen with square pixels where the
size allows it (176x208 and smaller: 2x and more); 240x320 uses all 480
lines and two screen pixels per column.

On a **TFT panel** (320x240, ST7789 or ILI9341) the picture is shown 1:1
in the middle of the panel, without scaling.  Screens up to 320x240 use the
panel in landscape; taller ones (240x320) switch it to portrait, so the
board is turned by 90 degrees for those games (`TFT_PORTRAIT_FLIP` turns
the picture the other way).  The game list and the menus are always in
landscape.  TFT firmware: `-TFT-ST7789`, `-TFT-ST7789V` (inverted colours)
and `-TFT-ILI9341`.

Controls:

| Keyboard | Gamepad | Phone |
|---|---|---|
| Arrows | D-pad | Up / Down / Left / Right |
| Enter, Space | A | Fire |
| F1 | Start | Left soft key |
| F2, Esc | B | Right soft key |
| 0..9 (main block or keypad) | X = 5, Y = 0 | 0..9 |
| Shift+8, keypad `*` | L | `*` |
| Shift+3, keypad `/` | R | `#` |
| Backspace, Delete | | Clear |
| F12 | Select | Emulator menu |
| Ctrl+Alt+Del | | Reboot |

The emulator menu pauses the game: resume, restart, change the screen size
(restarts the game) or the volume, go back to the game list, reboot.

Files on the SD card:

- `/j2me/` - the games
- `/.config/pico-j2me/<game>.ini` - per-game settings (screen size, volume)
- `/.config/pico-j2me/rms/<game>/` - saved games (record stores)

## Building

The firmware is built with the Raspberry Pi Pico SDK 2.2 (CMake + Ninja,
as from the Pico VS Code extension):

```sh
cmake -B build -G Ninja -DPICO_BOARD=murmulator2 -DHDMI=ON -DI2S=ON
cmake --build build
```

Options: `PICO_BOARD` (`murmulator`, `murmulator2`, `olimex-pico-pc`,
`waveshare_rp2350_pizero`), `VGA` / `HDMI` / `TFT` (Murmulator 1.x / 2.0
only; with `ILI9341` for that controller instead of ST7789, `TFT_INV` for
panels with inverted colours such as ST7789V, `TFT_PORTRAIT_FLIP`,
`TFT_MAX_MHZ` - the PIO clock limit, 126 by default: a 63 MHz SPI clock),
`I2S` (`OFF` = PWM),
`I2S_CS4334`, `USB_HID` (USB keyboard, on by default), `CPU_MHZ` (378 by
default, 252 on the PiZero).

`build_all.sh` / `build_all.bat` build every variant; the UF2 files land
in `bin/Release/`.

The class library is compiled with a **JDK 8** `javac` (newer ones cannot
target the CLDC class file version).  CMake uses the JDK 8 found through
`JAVA8_HOME` (or `JAVA_HOME`/`PATH`); without one it embeds the prebuilt
`classlib/classlib.jar`.  After changing `classlib/src`, refresh it with
`make prebuilt`.

### The desktop build and the tests

The headless desktop build (Linux, gcc + JDK 8) runs the same VM and class
library and is used by the tests:

```sh
make test                                            # the test suite
build/host/nanojava game.jar                         # run a MIDlet
build/host/nanojava --audio-out sound.wav game.jar   # and record its sound
build/host/nanojava --run-ms 5000 --screenshot shot.ppm \
    --keys 1000:-5,1100:-5:up game.jar               # scripted input
```

`--keys` takes `time_ms:keycode[:up]` entries using MIDP key codes (`-1`
to `-4` arrows, `-5` fire, `-6`/`-7` soft keys, `48`-`57` digits).

## Project layout

```
src/vm            the virtual machine
src/util          ZIP, inflate and PNG decoding
src/midp          MIDP natives, rasterizer, fonts, input queue, storage
src/audio         MIDI / tone / WAV decoding, the synthesizer
src/pal           platform interface (pal.h, pal_sys.h)
platform/rp2350   the RP2350 platform layer (start-up, launcher, video,
                  input, sound, PSRAM heap, FatFs files)
platform/host     the desktop platform layer (tests)
drivers/          Murmulator drivers (VGA, HDMI, PS/2, NES pad, I2S/PWM,
                  SD card, FatFs) from pico-gameboy
boards/           board pin headers
external/tlsf     the TLSF allocator (native heap in PSRAM)
classlib/src      the Java class library (classlib.jar: prebuilt copy)
tests/            tests and test MIDlets
tools/            font and waveform generators
docs/             architecture notes
```

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for how it works.

## Status and limitations

- The synthesizer uses simple waveforms (one per family of General MIDI
  instruments); MP3 and AMR are not supported.
- No networking; no touch screen / pointer input.
- No real-time clock: the wall clock starts at the build time.
- The interpreter is a straightforward switch-based one; the hot parts of
  the VM run from SRAM, the Java heap is in PSRAM behind the XIP cache.

## License

Nano Java (`src/`, `classlib/`, `platform/host`, `tests/`, `tools/`) is
dedicated to the public domain under [CC0 1.0 Universal](LICENSE), except
the built-in font (`src/midp/font_data.c`, DejaVu Sans, see
[docs/FONT_LICENSE](docs/FONT_LICENSE)).

The drivers in `drivers/` and the files taken from pico-gameboy keep their
own licenses (the PS/2 keyboard driver is GPL-2.0-or-later, which applies
to the firmware as a whole); TLSF is BSD-licensed (`external/tlsf`).
