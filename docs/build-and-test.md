# Build and test

## Dependencies

Build on a Linux host with Docker (Linux/amd64 containers), Make, Python 3.11+,
curl, `mkfs.ext4` and `debugfs`. Native validation additionally needs
`qemu-system-x86_64` and OpenSSL. Initialize the pinned Emacs submodule first.
The browser test driver is optional and described in [browser checks](web-browser-checks.md).

## Commands

| Command | Purpose |
| --- | --- |
| `make build` | Compile the pinned runtime and package the Linux system |
| `make run` | Build and boot the native reference with its retained home disk |
| `make web` | Build, verify vendor assets and serve the browser computer |
| `make wasm` | Rebuild the source-pinned browser emulator and firmware |
| `make web-serve` | Serve already built artifacts after verifying vendor assets |
| `make test` | Run fast host tooling and HTTP/asset tests |
| `make lint` | Check contribution language, format and the clean submodule pin |
| `make test-guest` | Build and accept the real native guest across two boots |
| `make check` | Run host checks, contribution checks and native guest acceptance |
| `make test-browser` | Exercise an already running Web console in Chromium |

Run guest acceptance from a clean committed checkout. It requires the build
manifest's revision and inputs to match. To include public DNS and HTTPS:

```sh
LINUX_INTERNET_URL=https://example.org/ make test-guest
```

## Reuse and identity

Docker caches the pinned compiler, archived Emacs source, configure/build and
system overlay separately. Linux source downloads are cached and SHA-256 checked
against `system/kernel.json`; the kernel is built in its own Docker stage. System packaging reuses artifacts only when input,
source pin, container identity and artifact hashes match. A parent revision
change refreshes the manifest without rewriting unchanged runtime bytes.
Existing native home disks are never formatted by the build.

The manifest records the parent revision and dirty flag, runtime fork pin,
container ID, build input hashes and output hashes. Native and browser reports
bind themselves to that manifest and their actual transcript/download bytes.
Compilation, emulator startup and interactive acceptance remain separate claims.

## Outputs

All generated files live under `build/`:

- `machine/manifest.json`: source and system artifact identity.
- `machine/initramfs.cpio.gz`: boot tools; device drivers are built into the kernel.
- `machine/runtime.squashfs`: read-only Emacs, libraries, source and manuals.
- `machine/data.ext4`: native user's disk; preserve it when cleaning builds.
- `machine/data-template.ext4`: fresh template copied into each browser VM.
- `machine/acceptance.json` and boot transcripts: native functional evidence.
- `web-browser/report.json` and screenshots: browser functional evidence.
- `web-vendor/`: hash-verified upstream browser assets.

There is no automatic clean target that deletes user data. Remove generated
build contexts or caches deliberately; retain any disk containing wanted files.

Public runtime sources, release identities and Pages deployment are described
in [releasing](releasing.md). `EMACSOS_HOSTED=1` enables browser acceptance
through the actual service-worker hosting path.
