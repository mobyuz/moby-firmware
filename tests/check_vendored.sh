#!/bin/sh
# Compares the vendored protocol files with the checksums in PROVENANCE.md.
set -eu
cd "$(dirname "$0")/../components/moby_protocol"
status=0
for path in $(sed -n 's/^[0-9a-f]\{64\}  //p' PROVENANCE.md); do
    want=$(grep "  $path\$" PROVENANCE.md | cut -c1-64)
    have=$(sha256sum "$path" | cut -c1-64)
    if [ "$want" != "$have" ]; then
        echo "vendored file changed: $path" >&2
        status=1
    fi
done
[ "$status" -eq 0 ] && echo "vendored protocol files match PROVENANCE.md"
exit "$status"
