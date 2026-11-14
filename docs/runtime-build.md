# Static runtime build

The runtime build uses the pinned `emacs/` source and a digest-pinned Alpine
3.22 x86-64 container. It produces a static, non-PIE GNU Emacs 30.2 executable
linked with musl 1.2.5, a matching portable dump, runtime Lisp/data and terminal
capabilities. This is a host build pipeline; it does not establish guest support.

```sh
git submodule update --init emacs
make runtime
```

Docker with BuildKit, Python 3 and an initialized clean Emacs checkout matching the staged
parent pin are required. The wrapper exports only tracked bytes from the exact pinned commit with
`git archive`; ignored or generated dependency files cannot seed the build.
Generated configure files, bytecode and build products stay under
`build/runtime/`; the dependency checkout remains unchanged. The
wrapper invokes Docker on every build and runs the resulting immutable image ID.
Docker reuses toolchain layers across worktrees. A local key covering the
submodule pin, image identity and recipes resets generated source and objects
when those inputs change. Otherwise GNU Make compiles incrementally. Every
build clears the installation staging tree, installs the matching runtime and
packages current parent Lisp. The container pins the complete installed package
closure in
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
| `build/runtime/manifest.json` | Parent/fork revisions, dirty-source indication, profile/toolchain/package identities and SHA-256 hashes for source export/executable/archive/every regular runtime file. |
| `build/runtime/elf-segments.txt` | Actual ELF segment layout from readelf. |

Runtime paths are `/usr/bin/emacs`,
`/usr/libexec/emacs/30.2/x86_64-pc-linux-musl/emacs.pdmp`,
`/usr/share/emacs/30.2/lisp`, `/usr/share/emacs/30.2/etc` and
`/usr/share/terminfo`. Minimal `/etc/passwd` and `/etc/group` describe uid/gid 0;
no account/security system is implied. The guest supplies writable `/work` and
`/tmp` separately. The runtime archive is not guest persistent storage.

Packaging excludes manuals, desktop integration, external Unix helper programs
and Lisp source files with matching bytecode. It retains upstream runtime data,
licenses and multilingual resources. Parent-owned `lisp/*.el`, when present,
are installed into `/usr/share/emacs/site-lisp`. Additional system Lisp can be
included explicitly with `python3 scripts/package-runtime.py --site-lisp PATH`;
all resulting bytes are included in the manifest. Runtime availability of an
upstream Lisp function does not promise a corresponding backend.

The packager limits the image to 128 MiB, at most 8190 entries and kernel-supported
path/component lengths. The kernel defines the guest disk/header format;
this pipeline supplies the standalone ELF and archive. `make build` installs
and repackages the runtime, including current parent Lisp, after incremental
compilation. See [build and test](build-and-test.md) for the Docker/toolchain
and worktree ownership model.
The recipe fixes inputs/configuration and records artifacts; it does not
claim byte-identical output across arbitrary build hosts or cached build trees.

## Validation scope

Building and inspecting the static executable establishes its build profile and
artifact identity. It does not establish that the guest implements every Linux
ABI operation available on the build host. Unsupported guest operations must
retain explicit errors; upstream source inspection and actual guest execution
bound the adapter's supported paths.

Run `make test-probes` for static-musl startup and memory-protection probes in
native QEMU, and `make test-guest` for the fourteen integrated Emacs checks.
Their logs and results are separate from the runtime build manifest. See
[guest acceptance](mvp.md) and [build and test](build-and-test.md).

Host compilation, host tests, kernel component probes, guest startup and
interactive guest acceptance are separate validation levels. A successful
runtime build alone makes no guest boot, file, timer, persistence or isolation
claim.
