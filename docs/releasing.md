# Browser releases and GitHub Pages

The public computer runs entirely in the visitor's browser. GitHub Pages serves
static files; it is not a shell server. The guest has no network device and its
home disk lasts for one powered-on session.

## What accompanies a release

The [browser release](https://github.com/suxiaogang223/emacsos/releases/tag/browser-demo-2026-11-18)
provides the emulator, firmware, Linux guest image and corresponding source at
the same download location. `web/assets.json`, `web/runtime.json` and
`web/sources.json` pin the downloadable bytes. `web/wasm-build.json` records the
emulator build inputs, output hashes and firmware inventory.

- `emulator-sources.tar.gz`: QEMU/Wasm, SeaBIOS, libfdt, key maps, Meson test
  subprojects, linked libraries, Emscripten runtime source, terminal source and
  build recipes. The unused precompiled QEMU firmware collection is excluded.
- `guest-sources.tar.gz`: every installed Alpine package's source, APKBUILD,
  patches and configuration, plus the exact GNU Emacs source archive. A package
  inventory records all installed versions, licenses and Alpine source commits.
  The Linux base source and stable patch are included separately, together with
  the parent kernel configuration and build policy.
- `guest-runtime.tar.gz`: kernel, small initramfs, read-only SquashFS runtime,
  fresh disk template and build manifest.
  It contains no native retained home disk.
- The tagged repository supplies the parent image recipe, init/session policy,
  Web presentation, tooling and license grant. The Emacs gitlink is unchanged.

Third-party components retain their licenses. Source archives preserve their
individual notices. See [licensing](licensing.md) and the
[browser notices](../web/licenses.html).

## Rebuild the emulator

On Linux, install Docker, Python 3.12+, curl and Make. From the release checkout:

```sh
make wasm
```

This verifies the pinned source archive, builds the Emscripten 3.1.50 toolchain
container and compiles QEMU and firmware with networking disabled. Linked
library sources are supplied locally. Installing general-purpose host tools
inside the toolchain container still needs network access. The container base
is pinned by digest; installed tool versions and the resulting image ID are
build-environment evidence, not a claim of byte-identical reproducibility.

Output goes into a new `build/wasm/` directory. Choose another output directory
when rebuilding after a failure or change; the command does not remove existing
builds. To use a source archive downloaded separately:

```sh
python3 scripts/build-wasm.py --archive /path/to/emulator-sources.tar.gz \
  --output build/wasm-rebuild
```

The archive hash and included recipes must match the checkout. The output
`artifacts/wasm-build.json` records the actual build. Compilation must be
followed by real guest acceptance before a new emulator is published.

## Prepare updated source materials

`system/wasm/inputs.json` pins repositories, commits, source archives and hashes.
To collect those inputs again, install Git and GNU patch, then run:

```sh
python3 scripts/prepare-wasm-sources.py
```

For the guest, build the system and install the authenticated GitHub CLI:

```sh
make build
python3 scripts/collect-guest-sources.py
```

The collector reads the actual initramfs package database, retrieves every
origin's exact Alpine recipe and patch blobs, and runs Alpine `abuild fetch`,
`verify` and `srcpkg`. It fails on incomplete recipes or checksum mismatches.
Include the full source archives, not only URLs or license reference files.
Archive the Emacs gitlink with `git -C emacs archive` and include it alongside
the Alpine source packages and inventory. Publish source and binary assets
together, update their manifests, and run host, native and browser acceptance
against the exact candidate before integrating its PR.

## Publish the static site

The Pages workflow follows a successful `Build and test` run on `main`, or an
explicit dispatch on `main`. It downloads the hash-pinned guest and emulator,
checks the guest's image inputs against the checkout, and stages only the Web
server's explicit public file allowlist. A system-source change requires a new
runtime release before the site can deploy. It cannot silently reuse a stale
guest. Public source and notice links accompany the computer.

`coi-serviceworker.js` is the unmodified MIT-licensed upstream script at commit
`7b1d2a092d0d2dd2b7270b6f12f13605de26f214`. It supplies COOP/COEP for hosts
that cannot configure response headers and reloads the first visit once. Its
scope is the project path. It does not cache a workspace or retain guest files.
Local hosting still supplies isolation headers directly.

Test the staged directory at a project subpath on an ordinary static server
without custom headers, then accept the deployed HTTPS site:

```sh
python3 scripts/package-web.py build/pages
EMACSOS_HOSTED=1 WEB_URL=https://suxiaogang223.github.io/emacsos/ make test-browser
```

Hosted mode checks actual isolation, boot, keyboard, Lisp, save/load, Eshell,
Dired and restart. Controlled missing-image and unsuitable-context injections
belong to the local-header run; service-worker requests bypass Playwright's
page routing. A successful workflow upload alone does not establish booting.
