# Browser machine

The browser console boots the current MVP disk image in QEMU compiled to
WebAssembly. xterm.js presents the emulated COM1 terminal at 80 columns by
24 lines. It runs the same trusted GNU Emacs runtime as the native QEMU MVP;
JavaScript handles the display, keyboard and machine controls. Application and
file operations remain in the guest.

The profile is fixed: one x86-64 machine, 1 GiB guest memory, a raw system disk,
no network device, no graphical adapter output, and no QEMU monitor. The browser
runtime's prebuilt Wasm heap reserves about 2.25 GiB of address space, in addition
to browser and image overhead. Use a desktop browser with WebAssembly threads
and enough available memory. Boot can take tens of seconds; the progress bar
becomes indeterminate after downloading the disk.

## Start locally

From the repository root:

```sh
make web
```

This builds the system image, verifies or downloads the pinned vendor assets into
`build/web-vendor`, and serves the console at `http://127.0.0.1:18082/`. Building
the image requires the Docker toolchains described in
[build and test](build-and-test.md). To serve an already built image without
invoking its build again:

```sh
make web-serve
```

The server defaults to loopback. Override `WEB_HOST` and `WEB_PORT` when needed,
or invoke `python3 scripts/serve-web.py --help`. It exposes an explicit list of
UI, vendor, image, manifest and license routes. It does not expose the repository
directory or accept uploaded files. Run `make test-web` for route, MIME,
isolation-header, TLS and asset-cache tests.

## Access from another computer

WebAssembly threads require `SharedArrayBuffer`, a secure browser context and
cross-origin isolation. The server sends `Cross-Origin-Opener-Policy:
same-origin` and `Cross-Origin-Embedder-Policy: require-corp`. Plain HTTP on a
remote LAN or Tailscale address does not satisfy the secure-context requirement.

Keep the server on loopback and forward it through SSH. For example, run this
on a Mac whose SSH target is `socrates@100.110.132.80`:

```sh
ssh -N -o ExitOnForwardFailure=yes \
  -L 127.0.0.1:18081:127.0.0.1:18082 socrates@100.110.132.80
```

Then open `http://127.0.0.1:18081/` on that Mac. The tunnel must remain open.
Replace the account, address or local port for another host.

Alternatively, serve HTTPS with a certificate trusted by the client:

```sh
python3 scripts/serve-web.py --host 0.0.0.0 --port 18082 \
  --cert /path/to/certificate.pem --key /path/to/private-key.pem
```

A reverse proxy or static host must preserve the isolation headers, serve
`.wasm` as `application/wasm`, and map the same allowlisted URLs. Serve vendor
files from the same origin. The local server has no authentication and is for
development; choose the listening address deliberately.

## Controls and lifecycle

POWER downloads the image and starts one emulator instance. Repeated clicks
cannot create another instance. Download and environment failures before
emulator initialization allow another POWER attempt. Once initialization has
begun, use RESTART to reload a fresh machine. Errors remain visible in the
status panel; there is no automatic restart.

The console becomes ready when the guest first displays its scratch buffer.
Normal Emacs keyboard input then reaches the raw serial terminal. On Mac,
Control supplies `C-` and Option supplies `M-`; `Esc`, then `x`, also invokes
`M-x`. The COMMAND, EVALUATE and CANCEL keys send `M-x`, `M-:` and `C-g` and
return focus to the terminal. The browser's shortcuts outside the terminal
retain their normal behavior.

RESTART asks for confirmation after emulator initialization. Leaving the page
requests the browser's standard unload warning; browsers may suppress it.
Guest runtime-exit and kernel-fault diagnostics update the machine status, as do
reported emulator exits or aborts. This observes the fixed MVP's serial output,
not a separate guest control protocol. A guest that hangs without a diagnostic
can remain in its last displayed state.

All writable guest files are volatile. Saving in `/work` preserves a file only
inside that running machine. Closing or restarting the page discards the whole
RAM filesystem. The DATA expansion bay is a visual reservation: there is no
writable disk mount, browser persistence, import/export, network API or guest
isolation beyond the current MVP. See [the MVP scope](mvp.md).

## Asset identity and licenses

`web/assets.json` pins the QEMU/Wasm artifact repository commit, individual
SHA-256 hashes, xterm.js 5.3.0 and xterm-pty 0.12.0. `make web-assets` accepts
cached bytes only when their hashes match, and downloads replacements through
a temporary file before atomic installation. The system image is independent
of those cached assets: `build/image/manifest.json` binds its guest components.

The vendor emulator is a prebuilt experimental upstream artifact. This project
has not reproduced its source/compiler build or audited the exact corresponding
source and firmware provenance. The local [browser notices](../web/licenses.html)
retain the available upstream reference notices and explain that limit.
`web/licenses/sources.json` records their URLs and hashes. Public distribution
requires resolving the remaining build and license-material provenance.
