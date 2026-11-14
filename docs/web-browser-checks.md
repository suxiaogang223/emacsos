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

The script checks isolation headers, five viewport widths, inactive disk-bay
semantics, download failure/retry, actual Wasm boot, Mac Option-x, direct command
keys, Ctrl-a, expression evaluation, source-file save/load, the application
manager, restart cancellation, fullscreen, guest exit, confirmed page restart
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
every supported browser, guest networking, disk mounting, persistence or fault
isolation. Native guest acceptance retains its own identity and scope.
