FROM alpine:3.22@sha256:49340b346798875b5592e5d420241f2f9cfba5f47e62ebb1446a1b34734e898f AS builder
COPY build-packages.lock /tmp/packages.lock
RUN xargs apk add --no-cache < /tmp/packages.lock
COPY source.tar /tmp/source.tar
ARG SOURCE_DATE_EPOCH
ENV SOURCE_DATE_EPOCH=$SOURCE_DATE_EPOCH
RUN mkdir /source && tar -xf /tmp/source.tar -C /source
WORKDIR /source
RUN ./autogen.sh && ./configure --prefix=/usr --without-all --without-x \
    --without-native-compilation --without-modules --without-libgmp \
    --with-gnutls --with-zlib --with-threads --with-file-notification=inotify \
    --with-dumping=pdumper --without-compress-install --disable-build-details
RUN make -j4 && make install DESTDIR=/runtime
COPY preload.el /prepare-preload.el
RUN src/emacs --batch -Q -l /prepare-preload.el \
    && rm -f src/emacs.pdmp && make -C src emacs.pdmp && make install DESTDIR=/runtime
RUN find info -type f -name '*.info' -delete \
    && make -j4 info INFO_OPTS=--split-size=131072 \
    && make install-info DESTDIR=/runtime INFO_OPTS=--split-size=131072

FROM alpine:3.22@sha256:49340b346798875b5592e5d420241f2f9cfba5f47e62ebb1446a1b34734e898f AS runtime
COPY packages.lock /tmp/packages.lock
RUN xargs apk add --no-cache < /tmp/packages.lock
COPY --from=builder /runtime/ /
RUN adduser -D -u 1000 -h /home/emacs emacs \
    && mkdir -p /etc/emacsos /usr/share/emacs/site-lisp /home /run \
    && rm -f /boot/initramfs-virt
COPY init /init
COPY inittab /etc/inittab
COPY boot start-emacs /etc/emacsos/
COPY desktop.el /usr/share/emacs/site-lisp/emacsos.el
RUN chmod 755 /init /etc/emacsos/boot /etc/emacsos/start-emacs

FROM builder AS packager
RUN apk add --no-cache squashfs-tools=4.6.1-r1
COPY --from=runtime / /guest/
COPY slim.sh /slim.sh
RUN sh /slim.sh /guest
RUN env -u SOURCE_DATE_EPOCH mksquashfs /guest/usr /usr.squashfs -comp gzip -noappend -all-root \
    -no-xattrs -processors 2 -mkfs-time "$SOURCE_DATE_EPOCH" -all-time "$SOURCE_DATE_EPOCH"

FROM scratch
COPY --from=packager /guest/ /
COPY --from=packager /usr.squashfs /boot/usr.squashfs
