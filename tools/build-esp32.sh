#!/bin/bash
# ESP32 固件构建/烧录（多板型）。
# 用法:
#   tools/build-esp32.sh <board> build          构建
#   tools/build-esp32.sh <board> flash <port>   构建+烧录（如 COM6 / /dev/ttyUSB0）
#   tools/build-esp32.sh <board> menuconfig     打开 menuconfig（Board selection 里可改板型）
#   board: cyd_2432s028r | cyd_2432s028r_plus | e32r35t | esp32s3-st7789-320_240-ec11 | esp32-st7735s-128_160-ec11 | esp32-st7789-320_240-ec11 | esp32-ILI9341-320_240-ec11 | esp32-ST7796-320_240-ec11 | esp32s3-st7796-480_320-xpt2046-ec11 | esp32s3-ILI9488-480_320-xpt2046-ec11 | esp32s3-ILI9341-320_240-xpt2046-ec11 | esp32c3-st7789-320_240-ec11 | jc8048w550 | JC4827W543C | ESP32-3248S035C | esp32s3-JLC-SZP | esp32s3-retro-go | all
#
#   ESP32-3248S035C → ESP32 + 3.5" 480x320 ST7796 SPI + GT911 → build-ESP32-3248S035C/ sdkconfig.ESP32-3248S035C（首次构建由 defaults 生成）
#
set -e
cd "$(dirname "$0")/../src/ports/esp32"
IDF_PS1="../../../tools/idf.ps1"

board_conf() {
    case "$1" in
        cyd_2432s028r)
            TARGET=esp32;   BDIR=build;            SDKCFG=sdkconfig
            DEFS="sdkconfig.defaults;sdkconfig.defaults.cyd_2432s028r" ;;
        cyd_2432s028r_plus)
            TARGET=esp32;   BDIR=build-cyd-plus;   SDKCFG=sdkconfig.cyd_2432s028r_plus
            DEFS="sdkconfig.defaults;sdkconfig.defaults.cyd_2432s028r_plus" ;;
        e32r35t)
            TARGET=esp32;   BDIR=build-e32r35t;    SDKCFG=sdkconfig.e32r35t
            DEFS="sdkconfig.defaults;sdkconfig.defaults.e32r35t" ;;
        # ...（其他板型略，保持原文件不动）
        esp32s3-st7789-320_240-ec11)
            TARGET=esp32s3; BDIR=build-ec11-knob-minimal; SDKCFG=sdkconfig.esp32s3-st7789-320_240-ec11
            DEFS="sdkconfig.defaults;sdkconfig.defaults.esp32s3-st7789-320_240-ec11" ;;
        esp32-st7735s-128_160-ec11)
            TARGET=esp32;   BDIR=build-ec11-knob-esp32;   SDKCFG=sdkconfig.esp32-st7735s-128_160-ec11
            DEFS="sdkconfig.defaults;sdkconfig.defaults.esp32-st7735s-128_160-ec11" ;;
        esp32-st7789-320_240-ec11)
            TARGET=esp32;   BDIR=build-ec11-knob-esp32-st7789; SDKCFG=sdkconfig.esp32-st7789-320_240-ec11
            DEFS="sdkconfig.defaults;sdkconfig.defaults.esp32-st7789-320_240-ec11" ;;
        esp32-ILI9341-320_240-ec11)
            TARGET=esp32;   BDIR=build-esp32-ili9341-ec11; SDKCFG=sdkconfig.esp32-ILI9341-320_240-ec11
            DEFS="sdkconfig.defaults;sdkconfig.defaults.esp32-ILI9341-320_240-ec11" ;;
        esp32-ST7796-320_240-ec11)
            TARGET=esp32;   BDIR=build-esp32-st7796-ec11; SDKCFG=sdkconfig.esp32-ST7796-320_240-ec11
            DEFS="sdkconfig.defaults;sdkconfig.defaults.esp32-ST7796-320_240-ec11" ;;
        esp32s3-st7796-480_320-xpt2046-ec11)
            TARGET=esp32s3; BDIR=build-esp32s3-st7796-ec11; SDKCFG=sdkconfig.esp32s3-st7796-480_320-xpt2046-ec11
            DEFS="sdkconfig.defaults;sdkconfig.defaults.esp32s3-st7796-480_320-xpt2046-ec11" ;;
        esp32s3-ILI9488-480_320-xpt2046-ec11)
            TARGET=esp32s3; BDIR=build-esp32s3-ili9488-ec11; SDKCFG=sdkconfig.esp32s3-ILI9488-480_320-xpt2046-ec11
            DEFS="sdkconfig.defaults;sdkconfig.defaults.esp32s3-ILI9488-480_320-xpt2046-ec11" ;;
        esp32s3-ILI9341-320_240-xpt2046-ec11)
            TARGET=esp32s3; BDIR=build-esp32s3-ili9341-ec11; SDKCFG=sdkconfig.esp32s3-ILI9341-320_240-xpt2046-ec11
            DEFS="sdkconfig.defaults;sdkconfig.defaults.esp32s3-ILI9341-320_240-xpt2046-ec11" ;;
        esp32c3-st7789-320_240-ec11)
            TARGET=esp32c3; BDIR=build-esp32c3-st7789-ec11; SDKCFG=sdkconfig.esp32c3-st7789-320_240-ec11
            DEFS="sdkconfig.defaults;sdkconfig.defaults.esp32c3-st7789-320_240-ec11" ;;
        jc8048w550)
            TARGET=esp32s3; BDIR=build-jc8048w550; SDKCFG=sdkconfig.jc8048w550
            DEFS="sdkconfig.defaults;sdkconfig.defaults.jc8048w550" ;;
        JC4827W543C)
            TARGET=esp32s3; BDIR=build-JC4827W543C; SDKCFG=sdkconfig.JC4827W543C
            DEFS="sdkconfig.defaults;sdkconfig.defaults.JC4827W543C" ;;
        ESP32-3248S035C)
            TARGET=esp32;   BDIR=build-ESP32-3248S035C; SDKCFG=sdkconfig.ESP32-3248S035C
            DEFS="sdkconfig.defaults;sdkconfig.defaults.ESP32-3248S035C" ;;
        esp32s3-JLC-SZP)
            TARGET=esp32s3; BDIR=build-esp32s3-jlc-szp; SDKCFG=sdkconfig.esp32s3-JLC-SZP
            DEFS="sdkconfig.defaults;sdkconfig.defaults.esp32s3-JLC-SZP" ;;
        esp32s3-retro-go)
            TARGET=esp32s3; BDIR=build-esp32s3-retro-go; SDKCFG=sdkconfig.esp32s3-retro-go
            DEFS="sdkconfig.defaults;sdkconfig.defaults.esp32s3-retro-go" ;;
        *) echo "unknown board: $1 (...|ESP32-3248S035C|...)" >&2; exit 1 ;;
    esac
}

idf() { powershell -NoProfile -ExecutionPolicy Bypass -File "$IDF_PS1" "$@"; }

build_one() {
    board_conf "$1"
    if [ ! -f "$SDKCFG" ]; then
        echo "== $1: set-target $TARGET (生成 $SDKCFG)"
        idf -B "$BDIR" -DSDKCONFIG="$SDKCFG" -DSDKCONFIG_DEFAULTS="$DEFS" set-target "$TARGET"
    fi
    echo "== $1: build ($BDIR)"
    idf -B "$BDIR" -DSDKCONFIG="$SDKCFG" -DSDKCONFIG_DEFAULTS="$DEFS" build
}

BOARD="${1:?board}"; ACT="${2:-build}"
if [ "$BOARD" = all ]; then
    # ...（原样保留）
    exit 0
fi

board_conf "$BOARD"
case "$ACT" in
    build)      build_one "$BOARD" ;;
    menuconfig) idf -B "$BDIR" -DSDKCONFIG="$SDKCFG" -DSDKCONFIG_DEFAULTS="$DEFS" menuconfig ;;
    flash)      build_one "$BOARD"; idf -B "$BDIR" -DSDKCONFIG="$SDKCFG" -p "${3:?port}" flash ;;
    *) echo "unknown action: $ACT" >&2; exit 1 ;;
esac