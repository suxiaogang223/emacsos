#!/bin/sh
set -eu
for path in /out/aports/*; do
    test -f "$path/.recipe-complete"
    cd "$path"
    abuild -F fetch
    abuild -F verify
    abuild -F srcpkg
    touch .verified
done
chown -R "$OUTPUT_UID:$OUTPUT_GID" /out
