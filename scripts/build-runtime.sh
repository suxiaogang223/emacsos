#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
image=emacsos-runtime:alpine3.22
pin=$(git ls-files -s emacs | awk '{print $2}')
actual=$(git -C emacs rev-parse HEAD)
if test "$pin" != "$actual" || test -n "$(git -C emacs status --porcelain)"; then
    echo 'Initialize a clean Emacs checkout matching the staged submodule pin.' >&2
    exit 1
fi
mkdir -p build/runtime
cache_key=$(
    { printf '%s\n' "$pin"; cat scripts/Dockerfile.runtime scripts/runtime-packages.lock scripts/build-runtime-inner.sh; } |
        sha256sum | awk '{print $1}'
)
if test ! -f build/runtime/build-key || test "$(cat build/runtime/build-key)" != "$cache_key"; then
    rm -rf build/runtime/source build/runtime/obj
    rm -f build/runtime/source.tar
    printf '%s\n' "$pin" > build/runtime/source-pin
    printf '%s\n' "$cache_key" > build/runtime/build-key
fi
if test ! -f build/runtime/source.tar; then
    git -C emacs archive --format=tar "$pin" > build/runtime/source.tar.part
    mv build/runtime/source.tar.part build/runtime/source.tar
fi
SOURCE_DATE_EPOCH=$(git -C emacs show -s --format=%ct HEAD)
export SOURCE_DATE_EPOCH
docker build --platform linux/amd64 -t "$image" -f scripts/Dockerfile.runtime scripts
docker run --rm --platform linux/amd64 --user "$(id -u):$(id -g)" \
    -e HOME=/tmp -e TZ=UTC -e SOURCE_DATE_EPOCH -e JOBS="${JOBS:-4}" \
    -v "$PWD:/work:z" -w /work "$image" ./scripts/build-runtime-inner.sh
python3 scripts/package-runtime.py
