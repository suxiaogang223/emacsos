# Core guest acceptance: develop and run Elisp inside emacsOS

The acceptance target is a fixed QEMU PC with 1 GiB RAM, BIOS boot, an IDE image
and COM1 terminal. One trusted static GNU Emacs 30.2 runs on the parent C kernel.
The runtime fork contains provenance changes only. The exact supported kernel
profile, littlefs file backend and application policy are documented in
[kernel bootstrap](kernel-bootstrap.md), [files](files.md),
[runtime build](runtime-build.md) and [application MVP](application-mvp.md).

## Reproduce

Initialize the pinned submodules, then use Docker, Make, Python 3 and QEMU:

```sh
make build
make check
make run
```

Acceptance requires a clean committed parent and initialized submodules.
The test creates an initialized DATA disk; it never selects a writable RAM
profile. `make test-storage` separately validates retention and read-only recovery.
`make build` builds the kernel and static runtime, installs parent-owned Lisp,
packs the image and records build identity. `make check` runs the host checks
and kernel probes, rebuilds as needed, and drives the real terminal command loop.
Use `make test-guest` for just native guest acceptance. The runner is
`tests/guest/check.py`, with source fixture `tests/guest/counter.el`.
`make run` launches the same image
for manual interaction; `C-a x` exits QEMU and `C-a a` sends Emacs a literal
`C-a`.

The script enters normal terminal text and `M-:` forms. It creates
`/home/user/projects/counter.el` with `find-file`, inserts source, saves, kills the buffer
and loads the saved file. A second edit checks the ordinary symlink lock,
backup contents and modified-state cleanup. It also checks directory, copy,
rename, metadata and deletion APIs, and preserves a real read-only filesystem
error with the unsaved buffer intact.

The saved application creates two timer-driven instances. The test visits
source through `*Processes*`, edits one instance's buffer, stops it and restarts
it through manager commands. It waits for real timer delivery and checks that
the old timer is removed, edited contents survive, the other instance advances,
and the restarted instance has a new identity and advances independently.
Assertions use output markers absent from the typed input to reject serial-echo
false positives. A failed assertion or guest exit fails acceptance.

## Evidence

Generated artifacts live under ignored `build/`:

| Artifact | Meaning |
| --- | --- |
| `runtime/manifest.json` | Pinned runtime source, toolchain/packages, executable, dump and installed archive identity. |
| `image/emacsos.img` | Complete kernel, static executable and runtime archive boot image. |
| `image/manifest.json` | Parent commit/tree, fork pin, dirty state, component hashes, build container IDs and machine profile. Build identity alone proves no guest behavior. |
| `image/guest.transcript` | Actual QEMU serial bytes for the latest acceptance attempt. |
| `image/guest-results.json` | Success/failure, passed assertions, parent/fork, image/build-manifest and transcript hashes. |

All recorded guest checks must complete. Only `passed: true` in the matching
acceptance record establishes that this script completed for that image. Host ERT, file sanitizer tests and the three
kernel probes give separate component evidence. Compilation and a startup
banner do not substitute for functional acceptance.

## Boundaries

All writable files use the DATA volume at `/etc`, `/home`, `/tmp` and `/var`.
Missing or rejected DATA leaves the machine in read-only recovery. The runtime
image is read-only. Cross-boot and browser retention require their separate
[storage acceptance](storage.md#validation-boundaries). Numeric IPv4 TCP has
its own [network acceptance](network.md#validation-boundaries), beyond this
core editing/application script. External subprocess support, service startup,
account isolation and per-application address spaces remain unimplemented.
The kernel and Emacs share privilege. Tracked resource counts are bookkeeping,
not enforced heap quotas. Application stop relies on callbacks returning;
non-yielding Lisp or a runtime fault can stop the whole environment.

This MVP implements the first application-management subset, not every A0
contract in the broader process model. Message queues, completion subscriptions
and deadline waits remain future work. DATA retention, native/browser networking
and K0 independent execution retain separate acceptance gates.

The original MVP established these editing/application checks using volatile
files. That writable RAM backend has been removed; current acceptance uses
littlefs while retaining the same ordinary GNU Emacs behavior checks.
