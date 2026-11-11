# emacsOS

emacsOS is a fresh operating-system project built around GNU Emacs and Elisp.
The intended public programming interface is standard Emacs Lisp, with file
and network capabilities supplied through a small platform abstraction layer.

This initial source contains only a minimal BIOS/x86-64 C-kernel scaffold for
serial output and a pinned GNU Emacs source submodule. **Emacs does not run on
this kernel yet.** Files, networking, persistence, services and a browser product
are not implemented. The scaffold is subject to compilation/static checks;
no emulator boot or functional acceptance is claimed by this reset.

## Source

```sh
git clone --single-branch --branch main --recurse-submodules https://github.com/suxiaogang223/emacsos.git emacsos
cd emacsos
# After changing an existing checkout:
git submodule update --init --recursive
```

`emacs/` pins [our Emacs fork](https://github.com/suxiaogang223/emacs), currently
GNU Emacs 30.2 plus provenance documentation. It carries no emacsOS runtime port.
The parent repository owns the kernel, future platform layer and system code.

## Build the scaffold

Use Make and Docker with Linux/amd64 support:

```sh
make container-image
```

With Clang, LLD, llvm-objcopy and NASM installed on the host, use `make image`.
`make kernel` builds the kernel; `make image` packages
`build/skeleton/emacsos.img`.
The intended manual launcher is `./scripts/run-qemu.sh` (or `make run`) with
`qemu-system-x86_64` installed. Boot execution has not been validated as part of
this initialization. These commands do not build or launch Emacs.

## Direction

Read the [overall architecture](docs/design.md),
[first runtime-port contract](docs/runtime-port.md),
[public programming interface](docs/programming-interface.md),
[roadmap](docs/roadmap.md) and [source layout](docs/source-layout.md).
The proposed route is standard Elisp APIs over GNU Emacs, static musl and a
limited Linux x86-64 ABI adapter, backed by a small typed PAL and our C kernel.
Architecture review comes first; no PAL or runtime port is implemented here.
Interactive evaluation, volatile file APIs, persistence and networking follow
as separate stages.

`main` starts with one initial commit. Previous implementations and their PR
history are outside this repository and do not establish capabilities here.

Public documentation, comments and community discussion use English. See
[CONTRIBUTING.md](CONTRIBUTING.md). Private user discussion follows the user's
language preference. GNU Emacs licensing remains in the submodule.
