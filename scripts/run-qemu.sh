#!/bin/sh
set -eu

cd "$(dirname "$0")/.."
exec qemu-system-x86_64 -M pc -m 128M -nic none -display none -monitor none \
    -serial stdio -no-reboot \
    -drive file=build/skeleton/emacsos.img,format=raw,if=ide,index=0,media=disk,snapshot=on "$@"
