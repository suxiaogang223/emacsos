# emacsOS

Your personal Lisp computer: GNU Emacs 30.2 on a small Linux system, presented
as a computer in the browser. Linux supplies devices, files, networking and OS
processes; Emacs supplies the user's programming and operating environment.
The project concentrates on the Web console and Emacs workflows.

## Start the browser computer

Install Docker with Linux/amd64 support, Make, Python 3.11+, e2fsprogs
(`mkfs.ext4` and `debugfs`) and curl. Initialize the pinned runtime source:

```sh
git clone --single-branch --branch main --recurse-submodules https://github.com/suxiaogang223/emacsos.git emacsos
cd emacsos
make web
```

Open `http://127.0.0.1:18082/` in a desktop browser and press POWER. The first
build compiles the pinned GNU Emacs source; subsequent builds reuse Docker
layers and verified system artifacts. WebAssembly threads, a secure context
(localhost or trusted HTTPS), cross-origin isolation and sufficient memory
are required. The prebuilt emulator reserves roughly 2.25 GiB plus page and
image overhead. See [browser hosting](docs/web.md) for remote access.

The display is a real 80 by 24 Emacs terminal. On Mac use Control for `C-` and
Option for `M-`; `Esc`, then `x`, also invokes `M-x`. The direct-access keys
open commands, evaluation, cancellation, files and Eshell. Create a file with
`C-x C-f`, save with `C-x C-s`, and execute Elisp with `M-x load-file`.
Your working directory is `~/work`.

The browser gets its own writable session disk. Closing or restarting the
page discards all session files. Browser retention, disk import/export and
network transport remain future work. The page's own asset downloads do not
provide guest networking.

## Native reference and checks

Install `qemu-system-x86_64` and OpenSSL for native guest validation:

```sh
make run
make test
# From a clean committed checkout:
make check
```

`make run` boots the same kernel and userspace with a separate host-backed
home disk at `build/machine/data.ext4`. Native acceptance uses a disposable
disk across two QEMU boots and checks ordinary editing, backups, locks,
subprocesses and certificate-verified synchronous HTTPS. Native data retention
is limited to clean shutdown and reopening the same disk; power-loss recovery
and browser retention are separate claims.

Use `make test-browser` against a running server for actual QEMU/Wasm boot,
keyboard, file, Eshell and lifecycle acceptance. See [browser checks](docs/web-browser-checks.md)
for the optional Playwright dependency and [build and test](docs/build-and-test.md)
for commands and evidence locations.

## Architecture and contribution

Read [design](docs/design.md), [roadmap](docs/roadmap.md),
[source ownership](docs/source-layout.md) and [acceptance](docs/mvp.md).
`emacs/` pins [the GNU Emacs fork](https://github.com/suxiaogang223/emacs);
system policy and Web code belong in the parent. The source fork currently
adds provenance only. The previous custom kernel and compatibility adapter
have been removed; their history is available in Git.

Use English for public project text; private discussion follows the user's
preference. See [CONTRIBUTING.md](CONTRIBUTING.md). GNU Emacs licensing remains
in the submodule; [runtime notices](web/licenses.html) describe the browser
emulator and guest components. Prebuilt emulator provenance remains a
separate distribution task.
