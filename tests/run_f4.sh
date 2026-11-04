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
