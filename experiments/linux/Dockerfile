FROM alpine:3.22@sha256:49340b346798875b5592e5d420241f2f9cfba5f47e62ebb1446a1b34734e898f

COPY packages.lock /tmp/packages.lock
RUN xargs apk add --no-cache < /tmp/packages.lock
RUN adduser -D -u 1000 -h /home/emacs emacs \
    && mkdir -p /etc/emacsos /usr/share/emacs/site-lisp /home /run \
    && rm -f /boot/initramfs-virt
COPY init /init
COPY inittab /etc/inittab
COPY boot start-emacs /etc/emacsos/
COPY desktop.el /usr/share/emacs/site-lisp/emacsos-linux.el
RUN chmod 755 /init /etc/emacsos/boot /etc/emacsos/start-emacs
