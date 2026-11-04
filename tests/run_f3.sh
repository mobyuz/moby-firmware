#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Builds and runs the F3 host tests with cc. The tests need cJSON and mbedTLS
# sources, taken from an ESP-IDF tree, the same copies the firmware links:
# $IDF_PATH, or .toolchain/esp-idf. The CI job runs this in espressif/idf.
set -eu
root=$(cd "$(dirname "$0")/.." && pwd)
idf=${IDF_PATH:-$root/.toolchain/esp-idf}
out=${MOBY_HOST_OUT:-${TMPDIR:-/tmp}/moby-f3}
cjson=$idf/components/json/cJSON
mbed=$idf/components/mbedtls/mbedtls
proto=$root/components/moby_protocol
mkdir -p "$out"

sh "$root/tests/check_vendored.sh"
sh "$root/tests/check_sources.sh"

# Third-party code is compiled as is, without our warning flags.
cc -std=c11 -w -I "$cjson" -c "$cjson/cJSON.c" -o "$out/cJSON.o"
for name in md sha256 platform_util constant_time; do
    cc -std=c11 -w -I "$root/tests/host" -I "$mbed/include" -I "$mbed/library" -c "$mbed/library/$name.c" -o "$out/$name.o"
done

for test in vectors f3; do
    cc -std=c11 -Wall -Wextra -Werror -pedantic \
        -I "$root/main" -I "$proto/include" -I "$root/tests/host" \
        -isystem "$cjson" -isystem "$mbed/include" \
        "$proto/c/moby_usb_codec.c" "$root/main/moby_json.c" "$root/main/moby_session.c" \
        "$root/main/moby_config.c" "$root/main/moby_link.c" "$root/main/moby_hmac.c" \
        "$root/main/moby_face.c" "$root/main/moby_font.c" "$root/tests/$test.c" \
        "$out/cJSON.o" "$out/md.o" "$out/sha256.o" "$out/platform_util.o" "$out/constant_time.o" \
        -lm -o "$out/$test"
    "$out/$test" "$proto/tests/fixtures"
done
