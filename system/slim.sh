#!/bin/sh
# Keep complete Emacs resources; the fixed-machine drivers are built in.
set -eu
root=$1
# SquashFS retains random access to complete Lisp sources and Info manuals.
strip --strip-debug "$root/usr/bin/emacs-"*
