# Browser acceptance

The browser driver boots a fresh Linux machine in real QEMU/Wasm. It records
actual serial writes, tests the user interface and matches the downloaded
kernel, initramfs, read-only runtime and template hashes against the served
system manifest.
Native networking and retained storage require separate guest acceptance.

Start `make web` in another terminal, then install the optional driver:

```sh
npm install --prefix build/web-browser-tools --no-save --package-lock=false playwright@1.56.1
build/web-browser-tools/node_modules/.bin/playwright install chromium
make test-browser
```

`EMACSOS_PLAYWRIGHT` selects another module path; `EMACSOS_BROWSER` selects an
installed Chromium executable. `WEB_URL` selects another server, and
`EMACSOS_WEB_REPORT` changes the default `build/web-browser` output directory.
The default computer URL is `http://127.0.0.1:18082/try/`. The document base
keeps guest and vendor requests at the hosting project root.
The driver runs Chromium without its sandbox for this trusted local test.

Checks include isolation headers, five viewport widths, session-disk wording,
missing download/retry, actual Linux/Emacs startup, simulated Mac Option-x,
command/cancel/evaluate keys, Ctrl-a and Ctrl-x Ctrl-e, ordinary file save/load,
Eshell, Dired, text-home buttons and return keys, editable Org notes and normal
save backups, a daily agenda with actual task content, a Lisp command callable
through `M-x`, source lookup, the Info manual, restart
cancellation, fullscreen, exit and confirmed restart.
An unsuitable browser context must fail visibly before VM initialization.
The driver also checks preloaded libraries, the verified static-image cache,
corrupt-cache recovery and a second boot with a fresh home disk. Its warm
restart timing includes deliberately refetching the corrupted kernel fixture.
Result markers are absent from typed input to reject serial echo false positives.

The report records Web source/dirty state, system and vendor manifests, actual
download hashes, startup time, passed checks and failures. Screenshots capture
initial, running, workspace and narrow layouts. Mac platform simulation does
not establish a physical Mac keyboard/browser combination. Passing this driver
does not establish other browsers, guest networking, browser persistence,
import/export, crash recovery or application isolation.

## Project homepage

Run the separate lightweight homepage checks against the same server:

```sh
EMACSOS_PLAYWRIGHT="$PWD/build/web-browser-tools/node_modules/playwright" \
  node tests/web/site.cjs http://127.0.0.1:18082/
```

The driver checks real screenshot loading, project-relative navigation, five
viewport widths, keyboard entry, visible storage/network limitations and the
absence of emulator, guest and isolation-script requests. Set
`EMACSOS_SITE_REPORT` to choose its report/screenshot directory. Run this and
the computer driver on a staged project subpath to check static hosting.
The computer driver captures six terminal screenshots named `home`, `notes`,
`agenda`, `files`, `shell` and `lisp` for reviewed gallery updates.
