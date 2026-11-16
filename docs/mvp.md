# Functional acceptance

The reference machine is a QEMU PC with 1 GiB RAM, COM1, a Linux kernel,
initramfs and separate virtio home disk. GNU Emacs 30.2 is compiled from the
pinned fork and runs as UID 1000. The browser uses the same kernel/userspace
with a fresh session disk and networking disabled.

## Native acceptance

From a clean committed checkout:

```sh
make check
# Optional public network check:
LINUX_INTERNET_URL=https://example.org/ make test-guest
```

The script drives the real Emacs command loop over serial. It checks keyboard
input, UID/version, ordinary save state, editing locks, backup contents,
loading saved Elisp, timers beside a running worker, forced worker stop and
survival of a worker segmentation fault. It performs certificate-verified
synchronous HTTPS against a local fixture with an unpredictable response,
and requires a certificate error when the fixture CA is untrusted.

It exits Emacs, synchronizes and unmounts the home disk, waits for QEMU poweroff,
then opens a new QEMU process with that same disk. Both edited source and backup
contents must match, and the saved Elisp must execute again. The acceptance disk
is disposable and distinct from the interactive user's disk.

## Evidence

`build/machine/manifest.json` identifies source, fork, container, build inputs
and system artifacts. `acceptance.json` binds its result to the manifest,
source, disk and both serial transcripts. Only a matching `passed: true` record
establishes completion. Compilation and host checks do not prove guest behavior.

[Browser acceptance](web-browser-checks.md) has a separate report with actual
Wasm boot, downloaded artifact identity, keyboard, session files and lifecycle.
It is required independently before claiming browser behavior.

## Boundaries

Native retention covers clean shutdown and reopening the same host-backed disk.
Power-loss durability, crash recovery and browser storage retention are untested.
A synchronous HTTPS check does not establish asynchronous HTTPS. A worker's
separate address space is not an application sandbox; processes share an account.
The main Emacs still has one command loop and heap. Browser networking remains
disabled; native results do not transfer to a browser transport.
