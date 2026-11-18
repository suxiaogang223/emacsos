# Browser computer

The Web console boots Linux and the pinned GNU Emacs 30.2 runtime inside
QEMU/WebAssembly. xterm.js displays the actual COM1 terminal at 80 by 24.
The page owns display, keyboard, download feedback and machine controls;
application and file operations execute in the guest.

## Open the public computer

Visit [emacsOS](https://suxiaogang223.github.io/emacsos/) and press **POWER**.
The first visit may reload once to prepare browser isolation. Use a desktop
browser and allow a few minutes for download and startup. The
[first-session guide](first-session.md) starts with a short Lisp expression.

## Start locally

```sh
make web
```

Open `http://127.0.0.1:18082/`. This builds the system, verifies/downloads the
pinned vendors and serves an explicit allowlist of UI, artifacts and notices.
`make web-serve` serves an existing build. Set `WEB_PORT` or `WEB_HOST` to
choose another address. The server never exposes the repository, native user
disk or build context and has no upload or remote-command API.

Use a desktop browser with WebAssembly threads and available memory. The
emulator reserves about 2.25 GiB of Wasm address space in addition to page and
image overhead. The guest has 1 GiB RAM, a Linux kernel and initramfs, one fresh
virtio session disk, COM1 and no network device or QEMU monitor.

## Access from another computer

SharedArrayBuffer requires a secure context and cross-origin isolation.
The server supplies COOP `same-origin` and COEP `require-corp`. Plain HTTP on
a remote LAN or Tailscale address is insufficient. Keep the server on loopback
and forward it through SSH from the browser's computer, for example:

```sh
ssh -N -o ExitOnForwardFailure=yes \
  -L 127.0.0.1:18081:127.0.0.1:18082 user@your-linux-host
```

Then open `http://127.0.0.1:18081/` locally. Adjust the account, remote address
and ports for your host. Alternatively use trusted HTTPS:

```sh
python3 scripts/serve-web.py --host 0.0.0.0 --port 18082 \
  --cert /path/to/certificate.pem --key /path/to/private-key.pem
```

A reverse proxy/static host must preserve the isolation headers, allowlisted
paths and Wasm MIME type. The development server has no authentication.

## Display and lifecycle

POWER downloads and verifies the kernel, initramfs and disk template against
the served system manifest, then starts one emulator. Download failures permit
a retry. After emulator initialization begins, RESTART reloads the page to
create a new machine. The progress indicator covers downloads, then becomes
indeterminate during boot. The real guest startup marker enables input.

The direct keys send `M-x`, `M-:`, `C-g`, `C-x d` and `M-x eshell` and restore
terminal focus. On Mac, Control supplies `C-` and Option supplies `M-`; `Esc`,
then `x`, also opens commands. FULL SCREEN expands the monitor. Guest exit,
kernel panic and emulator/worker failures update the status; an undiagnosed
hang can remain in its last state. Browser shortcuts outside the terminal
retain normal behavior.

The DATA bay describes a working session disk. It has no import/export or
browser retention controls. Saves remain available while this machine runs;
closing, refreshing or restarting discards them. Restart asks for confirmation,
and leaving requests the browser's unload warning, which browsers may suppress.
Native data retention does not imply browser retention.

## Asset identity and notices

`web/assets.json` pins the source-built QEMU/Wasm and firmware outputs,
xterm.js 5.3.0 and xterm-pty 0.12.0. The fetcher verifies cached files and installs
replacements atomically. System identity is separate in `build/machine/manifest.json`.
The browser verifies each system download before emulator initialization.

The [runtime release](releasing.md) supplies corresponding source archives,
exact dependency pins, build scripts, firmware inventory and notices.
`web/wasm-build.json` records the actual emulator build; `web/sources.json`
identifies the full source downloads. The old third-party prebuilt emulator
has been replaced. Guest packages and GNU Emacs retain their own licenses.

GitHub Pages uses a pinned, MIT-licensed Service Worker to supply isolation
headers on navigation. It reloads the initial visit once and does not cache
user files. Its scope stays inside `/emacsos/`; local hosting supplies headers
directly. See [publication and rebuilding](releasing.md).

See [browser acceptance](web-browser-checks.md) for actual guest tests and
screenshots. Browser networking and persistent storage require separate work.
