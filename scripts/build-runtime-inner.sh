#!/bin/sh
set -eu

out=/work/build/runtime
mkdir -p "$out"
# Configure/autogen generate files; keep the pinned dependency checkout clean.
if test ! -e "$out/source/configure"; then
    rm -rf "$out/source"
    cp -a /work/emacs "$out/source"
    rm -f "$out/source/.git"
    (cd "$out/source" && ./autogen.sh)
fi
mkdir -p "$out/obj"
cd "$out/obj"
if test ! -f Makefile; then
    CFLAGS='-O2 -g0 -fno-pie' LDFLAGS='-static -no-pie' \
        ../source/configure --prefix=/usr --without-all --without-x \
        --without-native-compilation --without-threads --without-modules \
        --without-libgmp --with-dumping=pdumper --with-file-notification=no \
        --without-compress-install --disable-build-details
fi
make -j"${JOBS:-4}"
rm -rf "$out/root"
make install DESTDIR="$out/root"
mkdir -p "$out/root/usr/share/terminfo"
cp -R /etc/terminfo/. "$out/root/usr/share/terminfo/"
apk info -v > "$out/packages.txt"
gcc --version > "$out/compiler.txt"
readelf -lW "$out/root/usr/bin/emacs" > "$out/elf-segments.txt"
./config.status --config > "$out/configure-arguments.txt"
printf '%s\n' "${SOURCE_DATE_EPOCH:-unset}" > "$out/source-date-epoch.txt"
