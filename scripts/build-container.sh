#!/bin/sh
set -eu

cd "$(dirname "$0")/.."
image=emacsos-skeleton:alpine3.22
docker build --platform linux/amd64 -t "$image" -f scripts/Dockerfile.build scripts
exec docker run --rm --platform linux/amd64 --user "$(id -u):$(id -g)" \
    -e HOME=/tmp -v "$PWD:/work:z" -w /work "$image" "$@"
