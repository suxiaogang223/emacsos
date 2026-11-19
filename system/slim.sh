#!/bin/sh
# Retain the fixed machine's drivers and complete, compressed Emacs resources.
set -eu
root=$1
version=$(basename "$root"/lib/modules/*)
modules=$(mktemp -d)
trap 'rm -rf "$modules"' EXIT

for module in virtio_pci virtio_blk virtio_net ext4 squashfs; do
    chroot "$root" /sbin/modprobe --show-depends -S "$version" "$module" \
        >> "$modules/dependencies"
done
awk '$1 == "insmod" {print $2}' "$modules/dependencies" | sort -u > "$modules/paths"
while IFS= read -r path; do
    mkdir -p "$modules/$(dirname "$path")"
    cp "$root/$path" "$modules/$path"
done < "$modules/paths"
rm -rf "$root/lib/modules/$version/kernel"
cp -a "$modules/lib/modules/$version/kernel" "$root/lib/modules/$version/"
chroot "$root" /sbin/depmod -a "$version"

# Sources remain available to find-function and Info through Emacs' gzip handler.
# Bytecode and the matching portable dump stay immediately loadable.
# Match upstream's compression policy: startup-only source such as subdirs.el
# has no bytecode partner and must remain directly discoverable during startup.
find "$root/usr/share/emacs" -type f -name '*.elc' | while IFS= read -r bytecode; do
    source=${bytecode%c}
    if test -f "$source"; then gzip -9n "$source"; fi
done
find "$root/usr/share/info" -type f ! -name 'dir' ! -name '*.gz' -exec gzip -9n {} +
find "$root/usr/share/man" -type f ! -name '*.gz' -exec gzip -9n {} +
strip --strip-debug "$root/usr/bin/emacs-"*
