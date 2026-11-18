# syntax = docker/dockerfile:1.5
# Adapted from ktock/qemu-wasm 0ef7b4e2814b231705d8371dd7997f5b72e70baf.
# Imported build logic retains the upstream QEMU licensing.

ARG PIXMAN_VERSION=0.42.2

FROM emscripten/emsdk@sha256:b6ea0e55fdc95be36427df6df7892d5e5e27f0440cfcf442a55f784aba09a4fa AS build-base
# Porting glib to emscripten inspired by https://github.com/emscripten-core/emscripten/issues/11066
ENV TARGET=/build/target
ENV CFLAGS="-O2 -matomics -mbulk-memory -DNDEBUG -sWASM_BIGINT -DWASM_BIGINT -pthread   -sASYNCIFY=1 "
ENV CXXFLAGS="$CFLAGS"
ENV LDFLAGS="-L$TARGET/lib -O2"
ENV CPATH="$TARGET/include"
ENV PKG_CONFIG_PATH="$TARGET/lib/pkgconfig"
ENV EM_PKG_CONFIG_PATH="$PKG_CONFIG_PATH"
ENV CHOST="wasm32-unknown-linux"
ENV MAKEFLAGS="-j4"
RUN apt-get update && apt-get install -y \
    autoconf \
    build-essential \
    libglib2.0-dev \
    libtool \
    pkgconf \
    ninja-build \
    python3-pip
RUN pip3 install meson==1.5.0
RUN mkdir /build
WORKDIR /build
RUN mkdir -p $TARGET

FROM build-base AS zlib-emscripten-dev
RUN mkdir -p /zlib
COPY zlib-1.3.1.tar.xz /tmp/zlib.tar.xz
RUN echo "38ef96b8dfe510d42707d9c781877914792541133e1870841463bfa73f883e32  /tmp/zlib.tar.xz" | sha256sum -c - && tar xJf /tmp/zlib.tar.xz -C /zlib --strip-components=1
WORKDIR /zlib
RUN emconfigure ./configure --prefix=$TARGET --static
RUN make install

FROM build-base AS libffi-emscripten-dev
RUN mkdir -p /libffi
COPY libffi.tar.gz /tmp/libffi.tar.gz
RUN echo "4f8748bead8eeaf7b282c5434c48d2033a732209ef56791ce67b95e22e994999  /tmp/libffi.tar.gz" | sha256sum -c - && tar xzf /tmp/libffi.tar.gz -C /libffi --strip-components=1
WORKDIR /libffi

RUN autoreconf -fiv
RUN LDFLAGS="$LDFLAGS -sEXPORTED_RUNTIME_METHODS='getTempRet0,setTempRet0'" ; \
    emconfigure ./configure --host=$CHOST --prefix=$TARGET --enable-static --disable-shared --disable-dependency-tracking \
    --disable-builddir --disable-multi-os-directory --disable-raw-api --disable-structs --disable-docs
RUN emmake make install SUBDIRS='include'

FROM build-base AS build-dev
RUN mkdir -p /stub
WORKDIR /stub
RUN <<EOF
cat <<'EOT' > res_query.c
#include <netdb.h>
int res_query(const char *name, int class, int type, unsigned char *dest, int len)
{
    h_errno = HOST_NOT_FOUND;
    return -1;
}
EOT
EOF
RUN emcc ${CFLAGS} -c res_query.c -fPIC -o libresolv.o
RUN ar rcs libresolv.a libresolv.o
RUN mkdir -p $TARGET/lib/
RUN cp libresolv.a $TARGET/lib/

RUN mkdir -p /glib
COPY glib-2.75.0.tar.xz /tmp/glib.tar.xz
RUN echo "6dde8e55cc4a2c83d96797120b08bcffb5f645b2e212164ae22d63c40e0e6360  /tmp/glib.tar.xz" | sha256sum -c - && tar xJf /tmp/glib.tar.xz -C /glib --strip-components=1

COPY --link --from=zlib-emscripten-dev /build/ /build/
COPY --link --from=libffi-emscripten-dev /build/ /build/

WORKDIR /glib
ENV CFLAGS="-Wno-error=incompatible-function-pointer-types -Wincompatible-function-pointer-types -O2 -matomics -mbulk-memory -DNDEBUG -pthread -sWASM_BIGINT  -sASYNCIFY=1"
ENV CXXFLAGS="$CFLAGS"
RUN <<EOF
cat <<'EOT' > /emcc-meson-wrap.sh
#!/bin/bash
set -euo pipefail
old_string="-Werror=unused-command-line-argument"
# emscripten ignores some -s flags during compilation with warnings. Meson checking phase fails when it sees these warnings.
new_string="-Wno-error=unused-command-line-argument"
cmd="$1"
shift
new_args=()
for arg in "$@"; do
  new_arg="${arg//$old_string/$new_string}"
  new_args+=("$new_arg")
done
"$cmd" "${new_args[@]}"
EOT
EOF
RUN <<EOF
cat <<'EOT' > /cross.meson
[host_machine]
system = 'emscripten'
cpu_family = 'wasm32'
cpu = 'wasm32'
endian = 'little'

[binaries]
c = ['bash', '/emcc-meson-wrap.sh', 'emcc']
cpp = ['bash', '/emcc-meson-wrap.sh', 'em++']
ar = 'emar'
ranlib = 'emranlib'
pkgconfig = ['pkg-config', '--static']
EOT
EOF
COPY pcre2-mirror.tar.bz2 pcre2_10.40-3_patch.zip /glib/subprojects/packagecache/
COPY gvdb.tar.gz /tmp/gvdb.tar.gz
RUN echo '14e4b83c4783933dc17e964318e6324f7cae1bc75d8f3c79bc6969f00c159d68  /glib/subprojects/packagecache/pcre2-mirror.tar.bz2' | sha256sum -c - \
    && mv /glib/subprojects/packagecache/pcre2-mirror.tar.bz2 /glib/subprojects/packagecache/pcre2-10.40.tar.bz2 \
    && echo '7d5c89913565aae16e10069a5d26c9af60c105ce21c77588751d48aab959f943  /tmp/gvdb.tar.gz' | sha256sum -c - \
    && rm -rf /glib/subprojects/gvdb && mkdir /glib/subprojects/gvdb \
    && tar xzf /tmp/gvdb.tar.gz -C /glib/subprojects/gvdb --strip-components=1
RUN meson setup _build --wrap-mode=nodownload --prefix=$TARGET --cross-file=/cross.meson --default-library=static --buildtype=release \
    --force-fallback-for=pcre2,gvdb -Dselinux=disabled -Dxattr=false -Dlibmount=disabled -Dnls=disabled \
    -Dtests=false -Dglib_assert=false -Dglib_checks=false
RUN sed -i -E "/#define HAVE_CLOSE_RANGE 1/d" ./_build/config.h
RUN sed -i -E "/#define HAVE_EPOLL_CREATE 1/d" ./_build/config.h
RUN sed -i -E "/#define HAVE_KQUEUE 1/d" ./_build/config.h
RUN sed -i -E "/#define HAVE_POSIX_SPAWN 1/d" ./_build/config.h
RUN sed -i -E "/#define HAVE_FALLOCATE 1/d" ./_build/config.h
RUN meson install -C _build

FROM build-base AS pixman-emscripten-dev
ARG PIXMAN_VERSION
RUN mkdir /pixman/
COPY pixman-0.42.2.tar.gz /tmp/pixman.tar.gz
RUN echo "ea1480efada2fd948bc75366f7c349e1c96d3297d09a3fe62626e38e234a625e  /tmp/pixman.tar.gz" | sha256sum -c - && tar xzf /tmp/pixman.tar.gz -C /pixman --strip-components=1
WORKDIR /pixman


RUN emconfigure ./configure --prefix=/build/target/
RUN emmake make -j4
RUN emmake make install
RUN rm /build/target/lib/libpixman-1.so /build/target/lib/libpixman-1.so.0 /build/target/lib/libpixman-1.so.$PIXMAN_VERSION

FROM build-base
COPY --link --from=zlib-emscripten-dev /build/ /build/
COPY --link --from=build-dev /build/ /build/
COPY --link --from=pixman-emscripten-dev /build/ /build/
WORKDIR /build/
COPY xterm-pty-0.12.0.tgz /tmp/xterm-pty.tgz
RUN echo '27b223d13f3c395c15ba91b18b70e7b0a10eabc81b716684eb4211f10b90c340  /tmp/xterm-pty.tgz' | sha256sum -c - \
    && mkdir -p /build/node_modules/xterm-pty \
    && tar xzf /tmp/xterm-pty.tgz -C /build/node_modules/xterm-pty --strip-components=1
CMD sleep infinity
