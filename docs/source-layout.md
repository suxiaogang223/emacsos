# Source layout and fork ownership

```text
emacsos/
├── emacs/                 # Pinned GNU Emacs 30.2 fork submodule
├── system/                # Linux image recipe, package locks, init and system Elisp
├── scripts/               # System build/native launcher, asset fetcher and web server
├── tests/guest/           # Real Linux guest acceptance across two QEMU boots
├── tests/tooling/         # Contribution and build contract checks
├── tests/web/             # HTTP boundary and real-browser acceptance
├── web/                   # Computer UI, terminal bridge and vendor identities/notices
└── docs/                  # Current design, roadmap, build and acceptance contracts
```

The parent owns the Linux image recipe, init/session policy, system Elisp, Web
UI and acceptance tools. Linux supplies kernel and backend behavior. No custom
kernel, syscall adapter, replacement libc or parallel application framework is
part of the current source tree.

`emacs/` pins the fork based on upstream GNU Emacs 30.2 commit
`636f166cfc86aa90d63f592fd99f3fdd9ef95ebd`. Its `EMACSOS.json` records provenance;
the fork currently changes provenance only. Build source is archived from that
exact clean gitlink. The compiler installs the executable, dump, Lisp and data
together; no distribution Emacs binary is substituted.

Initialize with `git submodule update --init --recursive`. Never float the pin
with `git submodule update --remote`. Necessary runtime changes belong in an
explicit fork branch and must be published before updating the parent gitlink.
Check the initialized checkout matches the staged pin. Ordinary system policy
belongs in the parent, even when installed under Emacs' Lisp directory.

`make build` produces `build/machine/vmlinuz`, `initramfs.cpio.gz`, a fresh
browser disk template and their manifest. `make run` uses the separate native
`data.ext4`; `make web` serves those artifacts through the pinned emulator.
Generated artifacts and downloaded vendors remain under ignored `build/`.
See [build and test](build-and-test.md) and [browser hosting](web.md).
