# Source layout and fork ownership

```text
emacsos/
├── emacs/                 # Pinned GNU Emacs fork submodule
├── kernel/
│   ├── boot.asm           # BIOS disk loader
│   ├── entry.asm          # x86-64 entry and fixed memory map
│   ├── main.c             # Kernel initialization and runtime entry
│   ├── loader.c           # Static ELF and startup ABI
│   ├── runtime.c          # Bounded syscall adapter and waits
│   ├── files.c / file_abi.c # Readonly namespace/DATA files and ABI translation
│   ├── block.c / littlefs_backend.c # DATA device and persistent objects
│   ├── storage_control.c  # Safe COM2 checkpoint barrier
│   ├── network.c / ne2000.c # Bounded IPv4 TCP streams and ISA device
│   ├── lwipopts.h / arch/ / lwip-libc/ # Freestanding lwIP port
│   ├── memory.c / hardware.c # Memory, serial and clocks
│   └── kernel.ld          # Link layout
├── lisp/                  # Trusted application policy and counter demo
├── third_party/littlefs/  # Pinned official littlefs submodule
├── third_party/lwip/      # Pinned official lwIP mirror submodule
├── tools/                 # Offline DATA template formatter
├── tests/                 # Kernel, Lisp, tooling, guest and browser checks
├── web/                   # Retro console, browser bridge and asset identities
├── scripts/               # Build, packaging, native launcher and browser tools
└── docs/                  # Current architecture and implementation plan
```

The fork is [suxiaogang223/emacs](https://github.com/suxiaogang223/emacs), based on
GNU Emacs 30.2 upstream commit
`636f166cfc86aa90d63f592fd99f3fdd9ef95ebd`.
Its `EMACSOS.json` records provenance. The initial fork adds provenance docs
only; no platform bridge, runtime port or system Lisp is present. Preserve GNU
history, notices, licensing and multilingual resources.

## Clone and update

```sh
git clone --single-branch --branch main --recurse-submodules https://github.com/suxiaogang223/emacsos.git emacsos
# Existing checkout:
git submodule update --init --recursive
```

The parent gitlinks pin the exact Emacs fork, official littlefs and official
lwIP mirror commits. Networking adapters/configuration belong to the parent.
`third_party/lwip.json` records release identity and compiled-source/license hashes;
see [networking](network.md) for the guest/host boundary.
The littlefs release identity and compiled-source hashes are recorded in
`third_party/littlefs.json`; its unchanged sources and license stay upstream.
Our filesystem adapter and freestanding configuration remain in `kernel/`. Do not float it implicitly with
`git submodule update --remote`. A source checkout is not a runtime build.
The default `make` target (`make build`) builds the complete system image at
`build/image/emacsos.img`. `make kernel` builds the kernel-only image and DATA template in Docker;
`make runtime` builds and packages static Emacs/musl. Docker shares toolchain
layers across worktrees, while each checkout owns its generated source and
build outputs. See [build and test](build-and-test.md) for prerequisites,
incremental behavior and focused commands.

`make test-guest` separately exercises the integrated guest subset; see
[acceptance](mvp.md) and [runtime build](runtime-build.md). `make web` builds and
serves the image through the pinned browser runtime; `make test-web` checks the
asset verification and allowlisted server. Browser bridge, page and notices
belong to the parent. `scripts/network.py` owns the automatic per-session
WebSocket-to-headless-QEMU SLIRP relay; guest TCP/IP stays in the kernel. Downloaded vendor assets stay in ignored
`build/web-vendor`; see [browser hosting](web.md).

Parent `lisp/` now contains trusted application policy and the counter example;
`tests/` contains their host ERT suite. See [application MVP](application-mvp.md)
for the policy subset and [MVP acceptance](mvp.md) for actual guest evidence.
The parent also enables GNU Emacs file sync through controlled storage policy.
These policies do not modify the runtime fork. Persistent backend and checkpoint
code belongs to the parent; see [storage](storage.md).

## Future changes

Necessary Emacs runtime/platform modifications belong to the fork. Ordinary
system Elisp and its tests should belong to the parent when introduced, rather
than accumulating as fork policy. The PAL and C backends belong to the parent;
the [architecture](design.md) selects static Emacs/musl with a limited Linux
x86-64 compatibility adapter over a typed PAL. The [runtime-port contract](runtime-port.md)
records required bootstrap capabilities; the MVP implements the initial bounded profile. Read-only runtime files precede writable system/user files. Future trusted
application management policy belongs to parent system Lisp; kernel execution,
scheduling and enforced process ownership belong to parent kernel code.
[The process model](process-model.md) defines their distinct phases.

A submodule normally checks out a detached commit. For a fork change, create an
explicit branch in `emacs/`, validate and publish that commit first, then stage
and commit the parent gitlink. A parent pin to an unpublished local commit is
not a reproducible handoff. Identify both revisions in reviews. Stage an updated
gitlink before running contribution checks; CI requires the initialized checkout
to match the pin and remain clean.

The source was initialized with one root commit; architecture changes and later
implementation use ordinary reviewed branches/PRs. Previous
implementations and reports are outside this repository; reuse requires explicit
design and review rather than assuming retired features still exist.
