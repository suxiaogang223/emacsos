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
    --with-gnutls --with-threads --with-file-notification=inotify \
    --with-dumping=pdumper --without-compress-install --disable-build-details
RUN make -j4 && make install DESTDIR=/runtime

FROM alpine:3.22@sha256:49340b346798875b5592e5d420241f2f9cfba5f47e62ebb1446a1b34734e898f
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
