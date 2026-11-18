FROM alpine:3.22@sha256:49340b346798875b5592e5d420241f2f9cfba5f47e62ebb1446a1b34734e898f
RUN apk add --no-cache abuild=3.15.0-r1 curl=8.14.1-r4
ENV REPODEST=/out/dist SRCDEST=/out/distfiles CARCH=x86_64 CBUILD=x86_64-alpine-linux-musl
RUN printf '%s\n' 'DISTFILES_MIRROR="https://distfiles.alpinelinux.org/distfiles/v3.22"' >> /etc/abuild.conf
