# emacsOS

emacsOS runs GNU Emacs 30.2 on its own x86-64 C kernel. Elisp is the public
programming interface. The first MVP provides serial-terminal editing and
evaluation, working files, and trusted cooperative applications managed from
`*Processes*`. A littlefs DATA disk retains `/work`, `/home` and `/tmp` across boots; the browser
automatically initializes and checkpoints it. Networking and isolated processes
remain future work.

## Build and run

Install Make, Python 3, Docker with BuildKit and Linux/amd64 support, and
`qemu-system-x86_64`. Initialize the pinned source dependency:

```sh
git clone --single-branch --branch main --recurse-submodules https://github.com/suxiaogang223/emacsos.git emacsos
cd emacsos
make build
make run
```

The first build compiles the kernel, static Emacs and its matching portable dump
in Docker toolchains. Later builds share Docker layers and use incremental
compilation in this worktree's build directory. Runtime installation and packaging
run each time. The image is `build/image/emacsos.img`; the launcher uses QEMU's
fixed PC profile, 1 GiB RAM, COM1 and no network. Exit QEMU with `C-a x`; send Emacs a literal `C-a` with `C-a a`.

Use `make test` for fast host tests and `make check` for host tests, contribution
checks, kernel probes and native guest acceptance. Host tests additionally need
Clang, Emacs and OpenSSL. Each worktree keeps separate build outputs while Docker
shares toolchain images and layers. See
[build and test](docs/build-and-test.md) for dependencies and focused commands.

In Emacs, use `M-:` to evaluate `(+ 20 22)`, `C-x C-f` to create
`/work/example.el`, and `C-x C-s` to save. Load your code with `M-x load-file`.
`/work` is saved on the DATA disk, which the native launcher creates on first
use at `build/data/emacsos-data.img`. `/home/user` and `/tmp` use that same
volume; temporary files are cleaned up by their applications. Missing or invalid
DATA starts read-only recovery, with no writable RAM fallback.
The installed runtime under `/usr` is read-only. See [persistent storage](docs/storage.md)
for synchronization, recovery and disk exchange.

For the bundled application example, evaluate:

```elisp
(require 'emacsos-counter)
(emacsos-counter-start)
(emacsos-counter-start)
(emacsos-processes)
```

Use `s` to stop a selected run, `r` to restart it with a fresh identity, `RET`
to inspect it, and `v` to visit its source. This is cooperative management in one
trusted Emacs; a non-yielding callback can block every application and the UI.

## Browser console

Run `make web` to build the same MVP image, fetch hash-pinned QEMU/Wasm and
xterm.js assets, and serve `http://127.0.0.1:18082/`. The browser runs its own
machine with an 80 × 24 serial terminal. Use POWER to boot; on Mac, Control
supplies `C-` and Option supplies `M-`. The page also provides direct command,
evaluation and cancel keys.

Use a desktop browser with WebAssembly threads and sufficient memory: the
prebuilt runtime reserves about 2.25 GiB plus image/browser overhead. HTTPS or
localhost and cross-origin isolation are required. For access from another
computer, use the [SSH forwarding instructions](docs/web.md#access-from-another-computer).
Plain HTTP on a remote address cannot run this runtime.

The retro computer page initializes an empty DATA disk automatically and retains
checkpoints in the browser for the same origin. Optional disk-image import and
export support moving your files. Guest saves and completed browser checkpoints
are distinct boundaries; retention failures remain visible. See
[browser controls and hosting](docs/web.md) for controls, limits and asset provenance.
The optional [browser checks](docs/web-browser-checks.md) exercise keyboard,
file and machine-control behavior against a fresh guest.

## Acceptance and architecture

From a clean committed checkout, run `make check`. It boots the actual
image, authors/saves/loads an Elisp application, checks normal backups, locks and
errors, then exercises two instances through the manager. Build and acceptance
manifests record source and artifact hashes. See [MVP acceptance](docs/mvp.md)
for evidence and scope, and [runtime build](docs/runtime-build.md) for build details.
Persistent-file acceptance additionally uses separate guest launches and a frozen
DATA copy; see [storage acceptance](docs/storage.md#validation-boundaries).
Host checks and kernel probes remain separate from guest functional acceptance.

Read the [architecture](docs/design.md), [runtime contract](docs/runtime-port.md),
[public interface](docs/programming-interface.md), [application model](docs/process-model.md),
[roadmap](docs/roadmap.md) and [source ownership](docs/source-layout.md).
The runtime uses static musl and a bounded Linux x86-64 ABI adapter over the
parent kernel and file backend. It does not provide general Linux compatibility.

`third_party/littlefs/` pins the official filesystem dependency; its sources
remain outside the parent tree. `emacs/` pins [our GNU Emacs fork](https://github.com/suxiaogang223/emacs), whose
only changes are provenance documentation. System policy stays in the parent.
Public documentation, comments and community discussion use English; private
user discussion follows the user's preference. See [CONTRIBUTING.md](CONTRIBUTING.md).
GNU Emacs licensing remains in the submodule. Historical implementations and PRs
outside this repository establish no current capabilities.
