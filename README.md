# emacsOS

emacsOS runs GNU Emacs 30.2 on its own x86-64 C kernel. Elisp is the public
programming interface. The first MVP provides serial-terminal editing and
evaluation, volatile working files, and trusted cooperative applications managed
from `*Processes*`. Networking, persistent storage and isolated processes are
future work.

## Build and run

Install Make, Python 3, Docker with Linux/amd64 support, and
`qemu-system-x86_64`. Initialize the pinned source dependency:

```sh
git clone --single-branch --branch main --recurse-submodules https://github.com/suxiaogang223/emacsos.git emacsos
cd emacsos
make mvp
make run
```

The first build compiles static Emacs and its matching portable dump in pinned
containers. Later builds reuse the runtime build directory. The image is
`build/mvp/emacsos.img`; the launcher uses QEMU's fixed PC profile, 1 GiB RAM,
COM1 and no network. Exit QEMU with `C-a x`; send Emacs a literal `C-a` with `C-a a`.

In Emacs, use `M-:` to evaluate `(+ 20 22)`, `C-x C-f` to create
`/work/example.el`, and `C-x C-s` to save. Load your code with `M-x load-file`.
Files under `/work`, `/tmp` and `/home/user` are writable **only until shutdown**.
The installed runtime under `/usr` is read-only.

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

## Acceptance and architecture

From a clean committed checkout, run `make check-mvp`. It boots the actual
image, authors/saves/loads an Elisp application, checks normal backups, locks and
errors, then exercises two instances through the manager. Build and acceptance
manifests record source and artifact hashes. See [MVP acceptance](docs/mvp.md)
for evidence and scope, and [runtime build](docs/runtime-build.md) for build details.
Host checks and kernel probes remain separate from guest functional acceptance.

Read the [architecture](docs/design.md), [runtime contract](docs/runtime-port.md),
[public interface](docs/programming-interface.md), [application model](docs/process-model.md),
[roadmap](docs/roadmap.md) and [source ownership](docs/source-layout.md).
The runtime uses static musl and a bounded Linux x86-64 ABI adapter over the
parent kernel and file backend. It does not provide general Linux compatibility.

`emacs/` pins [our GNU Emacs fork](https://github.com/suxiaogang223/emacs), whose
only changes are provenance documentation. System policy stays in the parent.
Public documentation, comments and community discussion use English; private
user discussion follows the user's preference. See [CONTRIBUTING.md](CONTRIBUTING.md).
GNU Emacs licensing remains in the submodule. Historical implementations and PRs
outside this repository establish no current capabilities.
