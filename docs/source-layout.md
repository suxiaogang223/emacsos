# Source layout and fork ownership

```text
emacsos/
├── emacs/                 # Pinned GNU Emacs fork submodule
├── kernel/
│   ├── boot.asm           # BIOS loader scaffold
│   ├── entry.asm          # x86-64 entry scaffold
│   ├── main.c             # Minimal C/serial scaffold
│   └── kernel.ld          # Link layout
├── scripts/               # Build, native launcher and contribution tools
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

The parent gitlink pins the exact fork commit. Do not float it implicitly with
`git submodule update --remote`. A source checkout is not a runtime build.
Current Make targets build only the kernel/image scaffold; Emacs build and
installation are future work.

## Future changes

Necessary Emacs runtime/platform modifications belong to the fork. Ordinary
system Elisp and its tests should belong to the parent when introduced, rather
than accumulating as fork policy. The PAL and C backends belong to the parent;
the [architecture](design.md) selects static Emacs/musl with a limited Linux
x86-64 compatibility adapter over a typed PAL. The [runtime-port contract](runtime-port.md)
records required bootstrap capabilities; concrete implementation remains future
work. Read-only runtime files precede writable system/user files. Future trusted
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
