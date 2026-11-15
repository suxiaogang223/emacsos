# Browser presentation checks

The browser checks boot a fresh machine and exercise the actual guest. They do
not attach to an existing terminal-server session or replace the full native
[MVP acceptance](mvp.md). The browser platform is simulated as Mac to exercise
xterm.js Option-as-Meta handling; this is not a manual test on macOS.

Start the local server with `make web` in another terminal. Install the optional
test driver into the ignored build directory:

```sh
npm install --prefix build/web-browser-tools --no-save --package-lock=false playwright@1.56.1
build/web-browser-tools/node_modules/.bin/playwright install chromium
EMACSOS_PLAYWRIGHT="$PWD/build/web-browser-tools/node_modules/playwright" \
  make test-browser
```

`EMACSOS_BROWSER` can select an already installed Chromium executable, and
`EMACSOS_WEB_REPORT` can select a different output directory. The default is
`build/web-browser`. Set `WEB_URL` to test another server URL. The test runner
uses Chromium without its sandbox, for
local development against this trusted guest and server.

The script checks isolation headers, five viewport widths, staged DATA import
controls, download failure/retry, actual Wasm boot, Mac Option-x, direct command
keys, Ctrl-a, expression evaluation, repeated allocation/GC with live roots, source-file save/load, the application
manager, restart cancellation, fullscreen, guest exit, save-before-restart, restored source without automatic execution
and an unsuitable browser context. It captures serial writes by instrumenting
the served xterm constructor inside the test browser; production has no debug
API. The guest output and state changes remain real.

The report records the web source revision/dirty flag, served guest build
manifest, hash of the image actually supplied to the browser, vendor asset
manifest, timings, passed checks and failures. Screenshots show the initial and
running machines, application list and narrow layout. Runtime/compiler source
provenance limits remain as described in [browser hosting](web.md).

These checks establish browser presentation behavior for the tested Chromium
environment. They do not establish a real Mac keyboard/browser combination,
every supported browser, guest networking or fault isolation. Native guest acceptance retains its own identity and scope.


For browser storage error paths, run `node tests/web/storage.cjs URL` with the
same driver environment. It uses real IndexedDB and Web Locks with a modeled
COM2 peer to check transaction aborts, independent byte ownership, serialized
barriers, stale sessions, invalid imports and session-only behavior. These checks
are separate from actual guest storage acceptance and do not substitute for it.

`node tests/web/disk-browser.cjs URL` runs separate real Wasm guests for automatic
retention across page close/reopen, single-writer contention, export/import in a
fresh browser context, rejected imports, and session-only operation when
IndexedDB is unavailable. `EMACSOS_DISK_REPORT` selects its output directory
(default `build/web-disk`); `EMACSOS_WEB_ROOT` identifies the local checkout served
at the URL when it differs from the driver checkout. The report separately
records driver source, selected server source, served build manifest and actual
SYSTEM/template/export hashes. These identities do not attest a remote server's
source. The exported image is retained in that output directory for native
interoperability checks.

For actual browser guest networking, run `make test-browser-network` against the
already-running server with the same driver environment. It requires a clean
checkout and a clean build manifest matching that exact source. It verifies the
actual fetched SYSTEM hash, automatic same-origin bridge, numeric IPv4 TCP echo
with delayed fragments, timer progress, DATA save, completed checkpoint and
restart with exact retained contents. Its localhost fixture receives real guest
TCP through the host SLIRP helper. Results default to `build/web-network`; set
`EMACSOS_WEB_REPORT` to retain a different attempt. This separate acceptance
does not imply DNS, guest TLS or unrestricted Linux socket compatibility.
