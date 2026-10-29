#!/bin/sh
# Build every supported RP2350 configuration of pico-j2me (Linux, macOS,
# Git Bash).  The Pico SDK comes from PICO_SDK_PATH, as for a normal build;
# a JDK 8 in JAVA8_HOME rebuilds the class library, otherwise the prebuilt
# classlib/classlib.jar is used.  The UF2 files land in bin/Release/.
#
#   ./build_all.sh                 all variants
#   ./build_all.sh murmulator2     the variants of one board
set -e

ROOT=$(cd "$(dirname "$0")" && pwd)
BUILD_TYPE=${BUILD_TYPE:-Release}
BUILD_ROOT=${BUILD_ROOT:-$ROOT/build/all}
if command -v ninja >/dev/null 2>&1; then GENERATOR=Ninja; else GENERATOR="Unix Makefiles"; fi

# board: video outputs: sound outputs (as wired on the board)
VARIANTS="
murmulator:VGA HDMI TFT-ST7789 TFT-ST7789V TFT-ILI9341:PWM I2S
murmulator2:VGA HDMI TFT-ST7789 TFT-ST7789V TFT-ILI9341:PWM I2S
olimex-pico-pc:HDMI:PWM
waveshare_rp2350_pizero:HDMI:I2S
"

build_one() {
    board=$1 video=$2 sound=$3
    tag=$board-$video-$sound
    vga=OFF hdmi=OFF tft=OFF ili=OFF inv=OFF i2s=OFF
    case $video in
        VGA) vga=ON ;;
        HDMI) hdmi=ON ;;
        TFT-ST7789) tft=ON ;;
        TFT-ST7789V) tft=ON inv=ON ;;
        TFT-ILI9341) tft=ON ili=ON ;;
    esac
    case $sound in I2S) i2s=ON ;; esac
    echo "[$tag]"
    cmake -S "$ROOT" -B "$BUILD_ROOT/$tag" -G "$GENERATOR" \
        -DCMAKE_BUILD_TYPE="$BUILD_TYPE" -DPICO_BOARD="$board" \
        -DVGA=$vga -DHDMI=$hdmi -DTFT=$tft -DILI9341=$ili -DTFT_INV=$inv \
        -DI2S=$i2s -DI2S_CS4334=OFF >/dev/null
    cmake --build "$BUILD_ROOT/$tag" --parallel >/dev/null
}

echo "$VARIANTS" | while IFS=: read -r board videos sounds; do
    [ -n "$board" ] || continue
    [ -z "$1" ] || [ "$1" = "$board" ] || continue
    for video in $videos; do
        for sound in $sounds; do
            build_one "$board" "$video" "$sound"
        done
    done
done

echo "UF2 files: $ROOT/bin/$BUILD_TYPE/"
