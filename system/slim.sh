#!/bin/sh
# Retain the fixed machine's drivers and complete Emacs resources.
set -eu
root=$1
version=$(basename "$root"/lib/modules/*)
modules=$(mktemp -d)
trap 'rm -rf "$modules"' EXIT

for module in virtio_pci virtio_blk virtio_net ext4 squashfs; do
    chroot "$root" /sbin/modprobe --show-depends -S "$version" "$module" \
        >> "$modules/dependencies"
done
# The baseline QEMU CPU has no SSE4.2. Use the generic CRC32C provider instead
# of kmod's optional Intel accelerator soft dependency.
awk '$1 == "insmod" && $2 !~ /\/crc32c-intel\.ko\.gz$/ && !seen[$2]++ {print $2}' \
    "$modules/dependencies" > "$modules/paths"
while IFS= read -r path; do
    mkdir -p "$modules/$(dirname "$path")"
    cp "$root/$path" "$modules/$path"
done < "$modules/paths"
rm -rf "$root/lib/modules/$version/kernel"
cp -a "$modules/lib/modules/$version/kernel" "$root/lib/modules/$version/"
find "$root/lib/modules/$version/kernel" -type f -name '*.gz' -exec gzip -d {} +
chroot "$root" /sbin/depmod -a "$version"
# Preserve dependency order and load only actual modules, excluding built-ins.
# BusyBox insmod then needs only musl before the /usr disk is mounted.
sed 's/\.gz$//' "$modules/paths" > "$root/etc/emacsos/modules"

# SquashFS compresses blocks while retaining random access to source and Info.
# File-level gzip would force Emacs to inflate whole manuals before seeking.
strip --strip-debug "$root/usr/bin/emacs-"*
