# Licensing and distribution

## Parent repository

Parent-owned emacsOS code is free software under the GNU General Public License,
version 3 or, at your option, any later version (**GPL-3.0-or-later**).
The full license text is in [LICENSE](../LICENSE).

This grant covers parent-owned source, documentation and original project
screenshots. Imported source, license texts and third-party components retain
their original licenses. The parent license does not relicense those components.

## Components

| Component | Source and license location |
| --- | --- |
| GNU Emacs | Pinned `emacs/` submodule; `emacs/COPYING` and individual file notices |
| Linux and Alpine packages | Pinned package closure in `system/packages.lock`; upstream package licenses |
| Compiler dependencies | `system/build-packages.lock`; kept outside the guest image |
| Browser terminal | xterm.js and xterm-pty; [runtime notices](../web/licenses.html) |
| QEMU/Wasm and firmware | Pinned source build in `system/wasm/`; hashes in `web/assets.json`; [runtime notices](../web/licenses.html) |

The source fork currently adds provenance only. Build scripts archive its exact
gitlink and install the executable, dump, Lisp and data from one build.

## Runtime releases

The public browser release replaces the old, unreconstructed prebuilt emulator
with a build from pinned QEMU/Wasm, Emscripten, linked libraries and firmware
sources. Its [release materials](releasing.md) provide complete source archives,
original notices, build instructions and output identities alongside the
binaries. `web/sources.json` pins source downloads; `web/wasm-build.json` records
the actual build. The guest source bundle covers every installed Alpine origin
and the exact Emacs gitlink.

QEMU is an aggregate of components under their original GPL, LGPL and
permissive licenses. SeaBIOS and the small QEMU boot ROMs are built separately
from source. The browser profile has no VGA or network device; unused prebuilt
firmware is excluded. The GPL-3.0-or-later grant for parent-owned work does not
change the licenses of Linux, QEMU, firmware, libraries or terminal components.

When updating any runtime binary, update the matching source and build materials
before publication. A license reference or artifact hash alone is insufficient.
The [runtime notices](../web/licenses.html) link the release source bundles and
component license texts. The [hosting guide](web.md) describes the browser
requirements and session lifetime.
