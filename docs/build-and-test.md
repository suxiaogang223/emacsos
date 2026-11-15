# Build and test

Run commands from the repository root. Building requires Git, Make, Python 3,
Docker with BuildKit and Linux/amd64 support and initialized `emacs/`, `third_party/littlefs/` and `third_party/lwip/` submodules. Run
`git submodule update --init --recursive` before building or running host file
tests. Docker
supplies the kernel and static Emacs compilation toolchains; host LLVM tools
are not required for the normal build. Native execution additionally needs
`qemu-system-x86_64`.

| Command | Purpose |
| --- | --- |
| `make` or `make build` | Build the complete system image. |
| `make run` | Build as needed and open the native serial console. |
| `make web` | Build as needed, verify browser assets and serve the browser console. |
| `make test` | Run host file, Lisp, tooling and web server tests without building a guest. |
| `make check` | Run host tests, contribution checks, kernel probes and native guest acceptance in order. |
| `make help` | Show the main entry points. |

`make kernel` builds the boot/kernel image and an initialized DATA template in Docker. `make runtime` builds
and packages static Emacs and its matching portable dump. These components feed
`make build`. Compilation, host tests, kernel probes and guest functional
acceptance retain separate results.

## Docker and incremental builds

Two Docker images provide the kernel and runtime toolchains. Each component
build invokes `docker build`, allowing Docker to reuse unchanged layers, then
runs the exact immutable image ID returned by that invocation. Local toolchain
images disable BuildKit attestations so repeated cached builds keep a stable ID. A mutable image
tag is a convenient name, not the toolchain identity used for execution.
Worktrees on the same Docker daemon share these image layers. Each worktree has
its own ignored `build/` directory, generated source and compilation output.

The runtime wrapper checks that the initialized Emacs submodule is clean and
matches the staged parent pin. It exports only that commit's tracked source.
A local build key covers the source pin, runtime image ID and build recipes;
changes reset the generated source and object directories. Otherwise GNU Make
handles incremental compilation in the existing object directory. Every runtime
build performs a fresh installation into its staging tree and packages that
installation together with current parent-owned Lisp.

Kernel builds and host file tests verify that littlefs is initialized, clean and
matches the recorded gitlink/source hashes before compilation. Kernel builds
also verify the lwIP pin and receipt. The image manifest records both exact
upstream commits. Kernel objects use ordinary Make
dependencies, including the kernel toolchain
image identity. Image assembly checks component identities and writes a build
manifest. Pinned inputs and recorded outputs do not establish byte-identical
compiler results across arbitrary hosts.

`scripts/image.py` owns complete-image assembly and its build identity record.
`scripts/qemu.py` defines the shared native QEMU machine profile for manual
execution, kernel probes and guest acceptance. Browser QEMU/Wasm keeps its own
documented platform configuration.

## Outputs

| Location | Contents |
| --- | --- |
| `build/kernel/` | Kernel objects, ELF, kernel-only image and probe evidence. |
| `build/runtime/` | Generated Emacs source/objects, fresh runtime installation, executable, archive and manifest. |
| `build/image/emacsos.img` | Complete kernel, static runtime and runtime-file image. |
| `build/image/data-empty.img` | Initialized 64 MiB littlefs DATA template; never a user disk. |
| `build/data/emacsos-data.img` | Default native user disk, created only when missing. |
| `build/image/storage/` | Separate-boot persistent guest transcripts, disk copies and results. |
| `build/network-probe/` | Native kernel network images, packet capture, transcripts and exact-source results. |
| `build/image/network/` | Actual Emacs network and HTTP/DATA reboot acceptance attempts and evidence. |
| `build/image/manifest.json` | Source/component hashes, toolchain identities and fixed machine profile. |
| `build/image/guest.transcript` | Actual serial bytes from the latest native guest acceptance attempt. |
| `build/image/guest-results.json` | Native guest acceptance outcome and source/image/transcript identities. |

A build manifest identifies artifacts; it does not establish guest behavior.

## Focused checks

Host tests require Clang with AddressSanitizer/UndefinedBehaviorSanitizer, Emacs
and OpenSSL for the TLS check. Kernel probes and guest acceptance require
Docker and native QEMU in addition to the build prerequisites.

| Command | Evidence |
| --- | --- |
| `make test-files` | Actual file backend on the host with sanitizers, including malformed archives. |
| `make test-lisp` | Warning-free byte compilation and application-policy ERT on host Emacs. |
| `make test-tooling` | Build, image and contribution-tool unit tests. |
| `make test-web` | Web server routes, isolation headers, TLS and asset verification. |
| `make test-network` | Real ABI objects with a fake stream PAL; socket errors/flags, readiness and ownership, without proving guest TCP. |
| `make test-probes` | Static-musl startup, deliberate RO/NX faults, storage device probes and actual native kernel TCP/absent-NIC probes. |
| `make test-guest` | Complete native guest editing, littlefs files and application lifecycle. |
| `make test-storage` | Sync/copy/reboot all DATA directories, cross-directory rename, stale locks and read-only recovery. |
| `make test-network-guest` | Actual Emacs numeric TCP, timers, HTTP/file sync and fresh-boot DATA acceptance; separate from DNS and browser acceptance. |
| `make test-browser` | Optional real-browser checks against an already running web server. |
| `make test-browser-network` | Actual pinned-Wasm TCP, timers, DATA save/checkpoint and restart through the automatic host bridge. |

Tests live under `tests/kernel`, `tests/lisp`, `tests/tooling`, `tests/guest`
and `tests/web`. Lisp compilation outputs stay under ignored `build/lisp`.
See [browser checks](web-browser-checks.md) for the optional Node.js/Playwright
driver installation and scope. See [networking](network.md) for native SLIRP and the automatic same-origin
browser bridge; the web server also needs native QEMU for online operation.
Browser checks are not included in `make check`
and are not replaced by web server unit tests.

Native guest acceptance requires a clean committed parent checkout and the
matching initialized submodule. Its checks drive the real guest through
`tests/guest/check.py`; the source fixture is `tests/guest/counter.el`. Its
transcript and results bind the exact image and source identities. See
[MVP acceptance](mvp.md) for the runtime and trust boundaries. All writable file acceptance now uses DATA.

CI uses one build/test workflow with separate host and guest jobs. The
contribution workflow separately checks repository/PR conventions, including
description-only PR edits. Playwright remains an optional local check.
