# Runtime build

The Linux system compiles the clean pinned GNU Emacs 30.2 fork in a multi-stage
Alpine container. `system/Dockerfile`, `build-packages.lock` and `packages.lock`
record the image recipe and complete compiler/guest package closures. The
Alpine base is digest-pinned.

The terminal profile enables GnuTLS, Lisp threads and Linux inotify, with a
portable dump and installed Lisp/data from the same build. GUI, native
compilation and dynamic modules are disabled. `--without-all` supplies the
starting profile; required Linux features are enabled explicitly. The actual
configure arguments are recorded in `build/machine/emacs.txt`.

The build archives tracked bytes from the exact gitlink, runs autogen/configure,
compiles and installs into a runtime stage. Compilers and source stay outside
the guest. System Elisp is installed from the parent. Runtime source changes
belong in the fork; account, session and application policy do not.

A Linux virtual-machine kernel and required userspace are installed from pinned
Alpine packages. BusyBox init owns startup and shutdown; Emacs runs as UID 1000
on ttyS0. The initramfs is a fresh volatile system root each boot. A separate
ext4 disk is mounted at `/home`. The native launcher supplies its host-backed
disk; the browser supplies a fresh copy of the template.

`make build` records source, container, inputs and component hashes. This is
build identity, not boot or feature acceptance. Native networking and retention
are exercised by [guest acceptance](mvp.md); browser behavior has independent
[checks](web-browser-checks.md). Package availability depends on the repositories,
and image byte reproducibility has not been established.
