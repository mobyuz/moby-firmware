#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Checks a built image against what it may contain. Run inside an activated ESP-IDF shell:
#   sh tests/check_image.sh [build dir] [default|ptt|f2]
#
#   default  the F3 image: the link, no microphone path at all (no I2S, no capture, no ring), no usb_pcm
#   ptt      the F4 image: the link and the push-to-talk capture, but not the F2 bench and no wake word
#   f2       the F2 bench image: the microphone bench and no link
set -eu
build=${1:-build}
mode=${2:-default}
elf=$build/moby_firmware.elf
cfg=$build/config/sdkconfig.h
fail=0
note() { echo "check_image: $*" >&2; fail=1; }
symbols=$(xtensa-esp32s3-elf-nm "$elf")
has() { echo "$symbols" | grep -Eiq "$1"; }
on() { grep -q "$1 1" "$cfg"; }

# Common: the console is UART0 and never the protocol port.
on CONFIG_ESP_CONSOLE_UART_DEFAULT || note "console is not UART"
grep -Eq 'CONFIG_ESP_CONSOLE_(USB_SERIAL_JTAG|SECONDARY_USB_SERIAL_JTAG) 1' "$cfg" && note "a USB console is on"

case "$mode" in
default)
    has 'moby_audio|moby_pcm|moby_capture|moby_ring|i2s_(new|channel)|i2s_std' && note "the default image contains audio, capture or I2S code"
    has ' T moby_link_step' || note "the link is missing"
    on CONFIG_MOBY_F2_MIC_TEST && note "the microphone bench is on"
    on CONFIG_MOBY_F4_PTT_AUDIO && note "push to talk is on in the default image"
    ;;
ptt)
    has ' T moby_link_step' || note "the link is missing"
    has ' T moby_capture_init' || note "the capture path is missing"
    has 'i2s_new_channel' || note "I2S is missing"
    has 'moby_audio_task' && note "the F2 bench code is in the push-to-talk image"
    has 'wake|vad_' && note "a wake word or VAD symbol is in the push-to-talk image"
    on CONFIG_MOBY_F2_MIC_TEST && note "the microphone bench is on"
    on CONFIG_MOBY_F4_PTT_AUDIO || note "push to talk is off"
    ;;
f2)
    has ' T moby_audio_task' || note "the bench is missing"
    has 'moby_link_step|moby_capture|moby_session' && note "the bench image contains the link or push to talk"
    on CONFIG_MOBY_F2_MIC_TEST || note "the microphone bench is off"
    ;;
*)
    note "unknown mode $mode"
    ;;
esac
[ "$fail" -eq 0 ] && echo "$mode image: ok"
exit "$fail"
