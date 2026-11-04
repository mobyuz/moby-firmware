#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Source checks for the privacy invariants of the default image. They read the
# sources, so they run anywhere. The built image is checked by check_image.sh.
set -eu
root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"
fail=0
note() { echo "check_sources: $*" >&2; fail=1; }

# 1. The F2 microphone bench is off by default and only its own sources reach the image.
grep -q 'default n' main/Kconfig.projbuild || note "Kconfig has no default n"
awk '/config MOBY_F2_MIC_TEST/{f=1} f&&/default/{print; exit}' main/Kconfig.projbuild | grep -q 'default n' \
    || note "MOBY_F2_MIC_TEST is not off by default"
! grep -q 'MOBY_F2_MIC_TEST' sdkconfig.defaults || note "sdkconfig.defaults enables the microphone bench"
! grep -q 'MOBY_F1_LED_TEST' sdkconfig.defaults || note "sdkconfig.defaults enables the LED bench"
grep -q '^CONFIG_MOBY_F2_MIC_TEST=y' sdkconfig.f2 || note "sdkconfig.f2 lost the bench switch"
grep -B1 -A2 'moby_audio.c' main/CMakeLists.txt | grep -q 'CONFIG_MOBY_F2_MIC_TEST' \
    || note "audio sources are not guarded by CONFIG_MOBY_F2_MIC_TEST"

# 2. Nothing of the protocol path touches audio, I2S or the microphone.
for file in main/moby_link.c main/moby_link.h main/moby_session.c main/moby_session.h main/moby_json.c \
            main/moby_config.c main/moby_transport.h main/moby_transport_usb_jtag.c main/moby_link_task.c; do
    [ -f "$file" ] || continue
    ! grep -Eq 'moby_audio|moby_pcm|driver/i2s|i2s_|MOBY_PIN_MIC' "$file" || note "$file touches the audio path"
done

# 3. The protocol channel is never the console: logs stay on UART0.
grep -q '^CONFIG_ESP_CONSOLE_UART_DEFAULT=y' sdkconfig.defaults || note "console is not UART by default"
grep -q '^CONFIG_ESP_CONSOLE_SECONDARY_NONE=y' sdkconfig.defaults || note "a secondary console is allowed"
! grep -Eq 'CONFIG_ESP_CONSOLE_(USB_SERIAL_JTAG|SECONDARY_USB_SERIAL_JTAG|USB_CDC)=y' sdkconfig.defaults \
    || note "a USB console is configured"

# 4. No log call names a key, nonce, proof or secret.
if grep -nE 'ESP_LOG[EWIDV]|printf|puts' main/moby_link.c main/moby_session.c main/moby_json.c main/moby_config.c \
        main/moby_link_task.c main/moby_transport_usb_jtag.c main/moby_config_nvs.c 2>/dev/null \
    | grep -Ei 'key|nonce|proof|secret|session_id' >&2; then
    note "a log call mentions a secret"
fi
exit "$fail"
