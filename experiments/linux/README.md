# Linux exploration

This experiment evaluates an Emacs-first system on a small Linux userspace.
It does not replace the own-kernel MVP or satisfy that MVP's F1/N/K gates.
The selected architecture and default build remain unchanged while this route
is evaluated. See [the exploration report](../../docs/linux-exploration.md).

## Run

Requirements: Docker with Linux/amd64 containers, native
`qemu-system-x86_64`, Python 3.11+, `mkfs.ext4`, `debugfs`, and OpenSSL.
The initial build downloads pinned Alpine packages. No host mounts or root
privileges are needed to run QEMU.

```sh
make linux-build
make linux-run
```

The guest boots directly into terminal Emacs as UID 1000. Use `C-x C-f` to
edit files in `~/work`, `M-x eshell` for external commands, and `C-x C-c` to
exit and shut down. A BusyBox init remains PID 1 for child reaping and shutdown.
The session is trusted and has no application sandbox.

`build/linux/data.ext4` is a dedicated 64 MiB writable home disk. Existing
disks are never reformatted by the build. Root userspace is unpacked from an
initramfs on each boot; changes outside `/home` are volatile. Persistence is
specified only across a clean Emacs exit, guest `sync`, successful unmount,
QEMU poweroff and a later boot of that same disk. Host power-loss durability,
interrupted writes, filesystem recovery and concurrent disk use are untested.

## Accept

Commit source changes before recording acceptance:

```sh
make linux-check
# Optionally require public DNS and HTTPS in addition to the local fixture:
make linux-check LINUX_INTERNET_URL=https://example.org/
```

Acceptance drives the real interactive command loop over COM1. It checks
typing, UID, ordinary saving, editing locks, backup contents, loading saved
Elisp, timers beside a running subprocess, forced subprocess termination,
survival of a subprocess segmentation fault, GnuTLS, certificate-verified
HTTPS against an unpredictable local response, rejection of an untrusted
certificate, clean shutdown and file retention/execution on a second boot.
Result markers are absent from terminal input to avoid accepting echoed text.

Only `build/linux/acceptance-data.ext4` is replaced by acceptance. It never
uses the interactive data disk. The HTTPS fixture listens on host loopback
and is reached at `10.0.2.2` through QEMU user networking. Its short-lived CA
is supplied only on the disposable test disk, never as a system trust anchor.
The optional public check requires HTTP 200 with certificate verification.

Evidence is separate from the own-kernel MVP:

| File under `build/linux/` | Meaning |
| --- | --- |
| `manifest.json` | Parent source, fork pin, input/artifact hashes, container identity and machine profile |
| `packages.txt`, `emacs.txt` | Installed package closure and actual Emacs build configuration |
| `first-boot.transcript`, `second-boot.transcript` | Actual serial output from both QEMU processes |
| `acceptance.json` | Result, exact source/manifest/disk/transcript identity and completed checks |

The kernel is Alpine `linux-virt=6.12.117-r0`; the runtime is the distribution's
`emacs-nox=30.1-r1`, not the project's pinned Emacs 30.2 fork. All 48 installed
packages are version-pinned in `packages.lock`; the Alpine base image is
digest-pinned. Package availability remains dependent on Alpine repositories.
Image byte reproducibility is not claimed.

## Observed network limitation

On this distribution build, timeout-enabled `url-retrieve-synchronously` and
an asynchronous `url-retrieve` left the local TLS connection in `connect`
state. The synchronous URL path without that optional timeout completed the
same certificate-verified HTTPS request. Acceptance uses that synchronous
path, bounded by the host's 60-second assertion deadline. It does not establish
working asynchronous HTTPS or a responsive UI during a blocking request.
Investigate this separately with the pinned Emacs 30.2 runtime before adoption.
