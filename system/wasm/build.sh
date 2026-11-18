#!/bin/bash
# Build the browser emulator and every firmware byte from the supplied sources.
set -euo pipefail
export SOURCE_DATE_EPOCH=1756965210
mkdir -p /out/qemu-build /out/firmware /out/optionrom
{ emcc --version; meson --version; gcc --version; python3 --version; ninja --version; node --version; dpkg-query -W; } > /out/tool-versions.txt
cd /out/qemu-build
flags='-O3 -g -Wno-error=unused-command-line-argument -matomics -mbulk-memory -DNDEBUG -DG_DISABLE_ASSERT -D_GNU_SOURCE -sASYNCIFY=1 -pthread -sPROXY_TO_PTHREAD=1 -sFORCE_FILESYSTEM -sALLOW_TABLE_GROWTH -sTOTAL_MEMORY=2300MB -sWASM_BIGINT --js-library=/build/node_modules/xterm-pty/emscripten-pty.js -sEXPORT_ES6=1 -sASYNCIFY_IMPORTS=ffi_call_js'
# Use dlmalloc for the threaded runtime; the original mimalloc path aborted during startup.
emconfigure /source/qemu/configure --static --target-list=x86_64-softmmu --cpu=wasm32 \
    --cross-prefix= --without-default-features --disable-download --enable-fdt=internal --disable-install-blobs --enable-system \
    --with-coroutine=fiber --enable-virtfs --extra-cflags="$flags" \
    --extra-cxxflags="$flags" \
    --extra-ldflags='-sMALLOC=dlmalloc -sEXPORTED_RUNTIME_METHODS=getTempRet0,setTempRet0,addFunction,removeFunction,TTY,FS'
emmake make -j4 qemu-system-x86_64
cp qemu-system-x86_64 /out/out.js
cp qemu-system-x86_64.wasm qemu-system-x86_64.worker.js /out/

# Firmware uses the native GNU compiler, separately from the Wasm toolchain.
unset CFLAGS CXXFLAGS LDFLAGS CPATH PKG_CONFIG_PATH EM_PKG_CONFIG_PATH
if [ ! -d /out/seabios ]; then cp -a /source/qemu/roms/seabios /out/seabios; fi
cp /source/qemu/roms/config.seabios-256k /out/seabios/.config
make -C /out/seabios EXTRAVERSION=-emacsos PYTHON=python3 oldnoconfig
make -C /out/seabios EXTRAVERSION=-emacsos PYTHON=python3 -j4
cp /out/seabios/out/bios.bin /out/firmware/bios-256k.bin
cd /out/optionrom
cp /source/qemu/pc-bios/optionrom/Makefile .
printf '%s\n' 'TOPSRC_DIR=/source/qemu' 'CC=gcc' 'OBJCOPY=objcopy' 'PYTHON=python3' > config.mak
make -j4
cp ./*.bin /out/firmware/
cd /out
python3 /emsdk/upstream/emscripten/tools/file_packager.py load-rom.data \
    --preload /out/firmware@/pack-rom --js-output=load-rom.js
sha256sum out.js qemu-system-x86_64.wasm qemu-system-x86_64.worker.js load-rom.js load-rom.data > SHA256SUMS
chown -R "$OUTPUT_UID:$OUTPUT_GID" /out
