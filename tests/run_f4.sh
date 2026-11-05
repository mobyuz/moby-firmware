#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Builds and runs the F4 host tests with cc: display text (UTF-8, Cyrillic glyphs) and the push-to-talk audio path.
# The audio tests need cJSON and mbedTLS sources from an ESP-IDF tree ($IDF_PATH, or .toolchain/esp-idf), the same
# copies the firmware links. The CI job runs this in espressif/idf.
set -eu
root=$(cd "$(dirname "$0")/.." && pwd)
out=${MOBY_HOST_OUT:-${TMPDIR:-/tmp}/moby-f4}
mkdir -p "$out"

# Display text needs only the font and the face.
cc -std=c11 -Wall -Wextra -Werror -pedantic -I "$root/main" \
    "$root/main/moby_font.c" "$root/main/moby_face.c" "$root/tests/text.c" -o "$out/text"
"$out/text"

# The audio path runs the real session and link, so it needs the same third-party code as the F3 tests.
idf=${IDF_PATH:-$root/.toolchain/esp-idf}
cjson=$idf/components/json/cJSON
mbed=$idf/components/mbedtls/mbedtls
proto=$root/components/moby_protocol
cc -std=c11 -w -I "$cjson" -c "$cjson/cJSON.c" -o "$out/cJSON.o"
for name in md sha256 platform_util constant_time; do
    cc -std=c11 -w -I "$root/tests/host" -I "$mbed/include" -I "$mbed/library" -c "$mbed/library/$name.c" -o "$out/$name.o"
done
cc -std=c11 -Wall -Wextra -Werror -pedantic \
    -I "$root/main" -I "$proto/include" -I "$root/tests/host" \
    -isystem "$cjson" -isystem "$mbed/include" \
    "$proto/c/moby_usb_codec.c" "$root/main/moby_json.c" "$root/main/moby_session.c" \
    "$root/main/moby_config.c" "$root/main/moby_link.c" "$root/main/moby_hmac.c" \
    "$root/main/moby_face.c" "$root/main/moby_font.c" "$root/main/moby_pcm.c" "$root/main/moby_ring.c" \
    "$root/tests/f4.c" \
    "$out/cJSON.o" "$out/md.o" "$out/sha256.o" "$out/platform_util.o" "$out/constant_time.o" \
    -lm -o "$out/f4"
"$out/f4"
