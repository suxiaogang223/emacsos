# Build and test

Run commands from the repository root. Make, Python 3, Docker with Linux/amd64
support and an initialized `emacs/` submodule are required to build. Native
execution needs `qemu-system-x86_64`. Host tests additionally need Clang with
AddressSanitizer/UndefinedBehaviorSanitizer, Emacs, and OpenSSL for the TLS check.

| Command | Purpose |
| --- | --- |
| `make` or `make build` | Build or reuse the complete MVP image. |
| `make run` | Build as needed and open the native serial console. |
| `make web` | Build as needed, verify browser assets and serve the browser console. |
| `make test` | Run host file, Lisp, tooling and web server tests. No guest build is needed. |
| `make check` | Run host tests, contribution checks, kernel probes and native guest acceptance in order. |
| `make help` | Show the main entry points. |

`make mvp` and `make check-mvp` remain aliases for existing callers. `make image`
still builds only the boot/kernel image using host tools; `make container-image`
builds that component in Docker. Compilation, host execution, kernel probes and
guest functional acceptance retain separate results.

## Incremental builds

The runtime build validates a clean submodule against the staged pin and exports
only that commit's tracked source. Its build key covers the pin, runtime
container recipe, package lock and inner build recipe. A changed key clears the
generated source/build cache. Missing, changed or incomplete installed artifacts
cause installation to run again; packaging recipe changes also reinstall.

On reuse, executable/archive/source hashes and the installed tree's entries,
file contents, modes and symbolic links are checked. Parent `lisp/*.el` inputs
are compared by name and content, so additions, changes and removals repackage
the runtime without compiling or reinstalling Emacs. Kernel changes rebuild
only the affected kernel objects and complete image. Unchanged archives and
images retain their modification times. Parent revision/dirty metadata is
refreshed separately without recompiling Emacs or rewriting an unchanged image.
This verifies cached inputs and artifacts; it does not claim bit-reproducible
compiler output across arbitrary hosts.

`scripts/image.py` owns image assembly, byte verification and the build identity
record. `scripts/qemu.py` defines the shared native QEMU machine profile for
manual execution, kernel probes and guest acceptance. Browser QEMU/Wasm keeps
its own documented platform configuration.

## Focused checks

| Command | Evidence |
| --- | --- |
| `make test-files` | Actual file backend on the host with sanitizers, including malformed archives. |
| `make test-lisp` | Warning-free byte compilation and application-policy ERT on host Emacs. |
| `make test-tooling` | Build/cache, image and contribution-tool unit tests. |
| `make check-web` | Web server routes, isolation headers, TLS and asset cache. |
| `make test-probes` | Static-musl startup and deliberate RO/NX page faults in native QEMU. |
| `make test-guest` | Complete native guest editing, files and application lifecycle. |
| `make runtime-smoke` | Optional host PTY diagnostic of the packaged Emacs/dump. |
| `make test-browser` | Optional real-browser checks against an already running web server. |

Tests live under `tests/kernel`, `tests/lisp`, `tests/runtime`, `tests/tooling`,
`tests/guest` and `tests/web`. Lisp compilation outputs stay under ignored
`build/lisp`; running host tests leaves the source tree clean. See
[browser checks](web-browser-checks.md) for Playwright installation and scope.
Browser checks are not included in `make check` and are not replaced by web
server unit tests.

Native guest acceptance still requires a clean committed parent checkout and
the matching initialized submodule. Its transcript and acceptance record bind
the exact image and source identities; a build manifest alone proves no guest
behavior. See [MVP acceptance](mvp.md) for the volatile-file and trust boundaries.

CI uses one build/test workflow with separate host and guest jobs. The
contribution workflow separately checks repository/PR conventions, including
description-only PR edits. Playwright remains an optional local check.
