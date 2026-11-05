#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Source checks for the privacy invariants. They read the sources, so they run anywhere.
# The built images are checked by check_image.sh.
set -eu
root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"
fail=0
note() { echo "check_sources: $*" >&2; fail=1; }

# 1. Every microphone path is off by default and only its own sources reach an image.
default_of() { awk -v c="config $1" '$0 ~ c {f=1} f&&/default/{print; exit}' main/Kconfig.projbuild; }
default_of MOBY_F2_MIC_TEST | grep -q 'default n' || note "MOBY_F2_MIC_TEST is not off by default"
default_of MOBY_F4_PTT_AUDIO | grep -q 'default n' || note "MOBY_F4_PTT_AUDIO is not off by default"
for option in MOBY_F2_MIC_TEST MOBY_F1_LED_TEST MOBY_F4_PTT_AUDIO; do
    ! grep -q "$option" sdkconfig.defaults || note "sdkconfig.defaults enables $option"
done
grep -q '^CONFIG_MOBY_F2_MIC_TEST=y' sdkconfig.f2 || note "sdkconfig.f2 lost the bench switch"
grep -q '^CONFIG_MOBY_F4_PTT_AUDIO=y' sdkconfig.f4 || note "sdkconfig.f4 lost the push-to-talk switch"
grep -B1 -A2 'moby_audio.c' main/CMakeLists.txt | grep -q 'CONFIG_MOBY_F2_MIC_TEST' \
    || note "audio sources are not guarded by CONFIG_MOBY_F2_MIC_TEST"
grep -B1 -A2 'moby_capture.c' main/CMakeLists.txt | grep -q 'CONFIG_MOBY_F4_PTT_AUDIO' \
    || note "capture sources are not guarded by CONFIG_MOBY_F4_PTT_AUDIO"
grep -A3 'config MOBY_F4_PTT_AUDIO' main/Kconfig.projbuild | grep -q 'depends on MOBY_F3_USB_LINK' \
    || note "push to talk does not depend on the link"

# 2. The pure protocol path never touches hardware, the platform or the microphone drivers.
for file in main/moby_link.c main/moby_link.h main/moby_session.c main/moby_session.h main/moby_json.c \
            main/moby_config.c main/moby_transport.h main/moby_msg.h main/moby_pcm.h main/moby_pcm.c \
            main/moby_ring.c main/moby_ring.h; do
    [ -f "$file" ] || continue
    ! grep -Eq '#include "(esp_|driver/|freertos/|soc/)|moby_audio\.h|moby_capture|moby_pins|i2s_|gpio_|MOBY_PIN_' "$file" \
        || note "$file touches hardware or the platform"
done
# The platform files outside the capture module never touch I2S.
for file in main/moby_link_task.c main/moby_transport_usb_jtag.c; do
    ! grep -Eq 'moby_audio\.h|driver/i2s|i2s_' "$file" || note "$file touches I2S"
done
# The link task names the capture path only under its option.
awk '/#ifdef CONFIG_MOBY_F4_PTT_AUDIO/{g=1} /#endif/{g=0}
     /MOBY_CAP_USB_PCM|moby_capture_|capture_set|capture_discard/ && !g && !/^#include/ {bad=1; print FILENAME": "$0 > "/dev/stderr"}
     END{exit bad}' main/moby_link_task.c || note "moby_link_task.c names the capture path outside CONFIG_MOBY_F4_PTT_AUDIO"
# usb_pcm is advertised nowhere else.
! grep -n 'capabilities *|\?= *[^0]' main/*.c | grep -v 'main/moby_link_task.c' | grep -q . \
    || note "a capability is set outside moby_link_task.c"

# 3. The session has exactly one place that turns capture on or off, so the LED, the microphone and the face
#    cannot disagree: the phase is assigned in enter_phase only, and capture_set is called there only.
[ "$(grep -c 'stream\.phase = ' main/moby_session.c)" -eq 1 ] || note "stream.phase is assigned in more than one place"
[ "$(grep -c 'ops\.capture_set(' main/moby_session.c)" -eq 1 ] || note "capture_set is called in more than one place"
[ "$(grep -c 'gpio_set_level(MOBY_PIN_RED_LED' main/moby_capture.c)" -eq 1 ] || note "the LED is set in more than one place"

# 4. No wake word, no VAD and no always-on listening exists at this stage.
! grep -Eiq 'MOBY_AUDIO_START_WAKE|wake_detected|esp_sr|wakenet|vad_(start|end|detect|process)' main/moby_session.c main/moby_link.c \
    main/moby_capture.c main/moby_link_task.c main/main.c || note "a wake word or VAD path is referenced"
[ "$(grep -c 'i2s_channel_enable' main/moby_capture.c)" -eq 1 ] || note "the I2S channel is enabled in more than one place"
grep -q 'i2s_del_channel' main/moby_capture.c || note "the I2S channel is never deleted"

# 5. The protocol channel is never the console: logs stay on UART0.
grep -q '^CONFIG_ESP_CONSOLE_UART_DEFAULT=y' sdkconfig.defaults || note "console is not UART by default"
grep -q '^CONFIG_ESP_CONSOLE_SECONDARY_NONE=y' sdkconfig.defaults || note "a secondary console is allowed"
! grep -Eq 'CONFIG_ESP_CONSOLE_(USB_SERIAL_JTAG|SECONDARY_USB_SERIAL_JTAG|USB_CDC)=y' sdkconfig.defaults \
    || note "a USB console is configured"

# 6. No log call names a key, nonce, proof or secret.
if grep -nE 'ESP_LOG[EWIDV]|printf|puts' main/moby_link.c main/moby_session.c main/moby_json.c main/moby_config.c \
        main/moby_link_task.c main/moby_transport_usb_jtag.c main/moby_config_nvs.c 2>/dev/null \
    | grep -Ei 'key|nonce|proof|secret|session_id' >&2; then
    note "a log call mentions a secret"
fi

# 7. Audio never goes to a log. The pure audio path has no print call at all (the session's log op takes fixed
#    state names), and the capture task logs error names only.
if grep -nE 'ESP_LOG|printf|puts' main/moby_ring.c main/moby_ring.h main/moby_pcm.h main/moby_link.c | grep -v snprintf | grep -q .; then
    note "the pure audio path prints"
fi
if grep -nE 'ESP_LOG|printf|puts' main/moby_capture.c | grep -v 'esp_err_to_name' | grep -q .; then
    note "moby_capture.c logs something other than an error name"
fi
exit "$fail"
