# Static runtime build

The runtime build uses the pinned `emacs/` source and a digest-pinned Alpine
3.22 x86-64 container. It produces a static, non-PIE GNU Emacs 30.2 executable
linked with musl 1.2.5, a matching portable dump, runtime Lisp/data and terminal
capabilities. This is a host build pipeline; it does not establish guest support.

```sh
git submodule update --init emacs
make runtime
make runtime-smoke
```

Docker, Python 3 and an initialized clean Emacs checkout matching the staged
parent pin are required. Generated configure files, bytecode and build products
stay under `build/runtime/`; the dependency checkout remains unchanged. The
wrapper invalidates its generated source/build cache when the submodule pin,
container recipe, package lock or inner build recipe changes. The container pins the complete installed package closure in
[scripts/runtime-packages.lock](../scripts/runtime-packages.lock) and records
actual installed versions in the artifact manifest. The base digest is pinned in
[scripts/Dockerfile.runtime](../scripts/Dockerfile.runtime). Package repository
retention is external: an unavailable pinned package fails the build rather than
silently selecting a different version.

The selected configure profile disables optional features, graphical display,
native compilation, Lisp threads, dynamic modules, external GMP and file
notifications. Interpreted/bytecompiled Lisp uses upstream mini-GMP. Terminal
capabilities use static ncurses. Disabling Lisp threads retains libc's own
thread-local storage and signal/mutex primitives. The ELF has no dynamic
interpreter; musl startup still needs its actual Linux ABI requests.

Portable dumping is retained because it is upstream's supported startup route
and avoids loading the complete preloaded Lisp set on each boot. The matching
executable/dump pair is mandatory. Its dump loader requires real anonymous and
fixed private file mappings, including PROT_NONE guard mappings; an adapter must
implement or reject those operations truthfully. A no-dump runtime is not a
parallel implementation in this change.

## Artifacts and filesystem contract

| Output | Purpose |
| --- | --- |
| `build/runtime/emacs` | Static ELF input for the kernel loader. |
| `build/runtime/root/` | Runtime filesystem staging tree. |
| `build/runtime/runtime.cpio` | Uncompressed newc archive, with deterministic entry order, zero archive timestamps and bounded paths/entry count. |
| `build/runtime/manifest.json` | Parent/fork revisions, dirty-source indication, profile/toolchain/package identities and SHA-256 hashes for executable/archive/every regular runtime file. |
| `build/runtime/elf-segments.txt` | Actual ELF segment layout from readelf. |

Runtime paths are `/usr/bin/emacs`,
`/usr/libexec/emacs/30.2/x86_64-pc-linux-musl/emacs.pdmp`,
`/usr/share/emacs/30.2/lisp`, `/usr/share/emacs/30.2/etc` and
`/usr/share/terminfo`. Minimal `/etc/passwd` and `/etc/group` describe uid/gid 0;
no account/security system is implied. The guest supplies writable `/work` and
`/tmp` separately. Neither the runtime archive nor the host smoke file is guest
persistent storage.

Packaging excludes manuals, desktop integration, external Unix helper programs
and Lisp source files with matching bytecode. It retains upstream runtime data,
licenses and multilingual resources. Parent-owned `lisp/*.el`, when present,
are installed into `/usr/share/emacs/site-lisp`. Additional system Lisp can be
included explicitly with `python3 scripts/package-runtime.py --site-lisp PATH`;
all resulting bytes are included in the manifest. Runtime availability of an
upstream Lisp function does not promise a corresponding backend.

The packager limits the image to 128 MiB, at most 8190 entries and kernel-supported
path/component lengths. The root coordinator owns the guest disk/header format;
this pipeline supplies the standalone ELF and archive. Repackaging is explicit
when parent Lisp changes; an old manifest must not be used as evidence for new
source. The recipe fixes inputs/configuration and records artifacts; it does not
claim byte-identical output across arbitrary build hosts or cached build trees.

## Host evidence and ABI discovery

`make runtime-smoke` launches the packaged executable/dump in a real host PTY
with controlled startup (`-Q -nw`) and runtime data paths. It types text, runs a
command-loop timer, evaluates Lisp and writes/reads a temporary host file before
exiting. Results and the terminal transcript remain under `build/runtime/`.

To trace the same smoke inside the build container:

```sh
docker run --rm --platform linux/amd64 \
  --user "$(id -u):$(id -g)" -v "$PWD:/work:z" -w /work \
  emacsos-runtime:alpine3.22 python3 scripts/smoke-runtime.py --trace
```

This produces `host-syscalls.trace`, using the container's real Linux kernel.
A trace is evidence for exercised paths only. It includes host process creation
by the harness and may include optional probes that should fail on emacsOS.
For example, musl can attempt a Unix-domain nscd connection; this is not a reason
to implement a general socket stack for this MVP. Compare requested semantics,
results and runtime behavior when deriving the bounded guest ABI.

Host compilation, host PTY success, kernel component probes, guest startup and
interactive guest acceptance are separate validation levels. This document
makes no guest boot, file, timer, persistence or isolation claim.
