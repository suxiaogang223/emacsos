# Browser machine

The browser console boots the current MVP disk image in QEMU compiled to
WebAssembly. xterm.js presents the emulated COM1 terminal at 80 columns by
24 lines. It runs the same trusted GNU Emacs runtime as the native QEMU MVP;
JavaScript handles the display, keyboard and machine controls. Application and
file operations remain in the guest.

The profile is fixed: one x86-64 machine, 1 GiB guest memory, a raw system disk,
an NE2000 ISA network device when the same-origin bridge is available, no
graphical adapter output, and no QEMU monitor. The browser
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
directory or accept uploaded files. The `/network` endpoint additionally
provides a same-origin binary WebSocket upgrade and starts a private headless
QEMU SLIRP relay automatically. Native `qemu-system-x86_64` is required for this
online path; its absence permits offline boot. Run `make test-web` for route, MIME,
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
files from the same origin, and forward `/network` WebSocket upgrades for guest
networking. HTTPS uses WSS automatically; SSH forwarding needs no additional
port. The local server has no authentication and is for
development; choose the listening address deliberately.

## Controls and lifecycle

POWER downloads the image and starts one emulator instance. Repeated clicks
cannot create another instance. Download and environment failures before
emulator initialization allow another POWER attempt. Once initialization has
begun, RESTART checkpoints DATA before reloading a fresh machine. Errors remain visible in the
status panel; there is no automatic restart.

The console becomes ready when the guest first displays its scratch buffer.
Normal Emacs keyboard input then reaches the raw serial terminal. On Mac,
Control supplies `C-` and Option supplies `M-`; `Esc`, then `x`, also invokes
`M-x`. The COMMAND, EVALUATE and CANCEL keys send `M-x`, `M-:` and `C-g` and
return focus to the terminal. The browser's shortcuts outside the terminal
retain their normal behavior.

RESTART asks for confirmation after emulator initialization, freezes DATA and
waits for a completed browser checkpoint before reloading. A failed checkpoint
keeps the page open with an error. DISCARD & RESTART explicitly reopens the last
completed checkpoint. Leaving the page requests the browser's standard unload
warning; browsers may suppress it.
Guest runtime-exit and kernel-fault diagnostics update the machine status, as do
reported emulator exits or aborts. Machine diagnostics observe COM1 serial output; DATA
checkpoints use a separate COM2 control protocol. A guest that hangs without a diagnostic
can remain in its last displayed state.

## Network link

POWER automatically connects the guest Ethernet transport when the server
advertises it. The network indicator reports link status separately from boot
and DATA. A link connection does not prove a TCP destination is reachable.
Closing the page or stopping the server tears down its relay. After a link
disconnect, restart to reconnect; existing streams do not resume transparently.
Offline boot still supports Lisp and the available files.

Each connection owns a separate headless host QEMU SLIRP session, with at most
four sessions. It requires no manual proxy or privileged TAP setup. The guest
runs TCP/IP through NE2000/lwIP; the browser forwards Ethernet rather than doing
HTTP requests on the guest's behalf. `10.0.2.2` identifies the server host, which
may differ from the browser computer. Origin checks do not authenticate users
or prevent access to services reachable by that host. See [networking](network.md)
for supported APIs, resource bounds, deployment and validation limits.

## DATA controls

POWER automatically selects the retained disk for this browser origin, or creates
an initialized empty 64 MiB littlefs disk on first use. The guest mounts it at
`/etc`, `/home`, `/tmp` and `/var`; only saved and checkpointed files are retained.
Unsaved buffers remain in memory. Missing or rejected DATA enters read-only
recovery; there is no writable RAM fallback. No setup or
manual export is required for ordinary use.

SAVE NOW requests an explicit checkpoint. Automatic polling also checkpoints
changed DATA after guest synchronization. The status distinguishes pending,
saved, session-only and failed states. An Emacs save reaches the emulated DATA
disk; only a completed IndexedDB transaction means it is retained in this
browser. Quota failures and unavailable storage preserve the prior checkpoint
and remain visible. RESUME confirms guest state after an uncertain storage
barrier. No successful save is inferred from terminal text.

EXPORT downloads a consistent full DATA image. Before POWER, IMPORT stages a
64 MiB image, and NEW DISK stages an initialized empty disk. The guest validates
the staged volume before it can replace the retained checkpoint. Imports never
cause user Lisp to load automatically. There is no hot-swap or host-directory
mount. The raw image can also be used by the native launcher.

Retention belongs to the browser profile and origin (scheme, host and port).
Changing an SSH local port therefore selects a different store. Clearing site
data or browser eviction can remove it; export supplies a portable backup. A
Web Lock prevents simultaneous writers in two tabs. Browser environments without
ownership/retention support use an explicitly labeled session-only disk. See
[storage](storage.md) for sync, recovery and failure boundaries. This adds no
fault isolation; networking has its own transport and acceptance gates.

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
