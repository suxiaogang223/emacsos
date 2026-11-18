# Project screenshots

These are original, unretouched screenshots of the real emacsOS browser
computer, captured by `tests/web/browser.cjs` on November 18, 2026.
They are included under the parent repository's GPL-3.0-or-later license.

| File | Actual state |
| --- | --- |
| `workspace.png` | Linux has booted; GNU Emacs 30.2 displays the welcome workspace |
| `files.png` | Dired displays a file saved and loaded inside the guest |
| `power-on.png` | The computer's powered-off page before boot |

Capture Web source: `267ff6804afacdd9461dc249f58995b4b1b17177`.
Guest image source: `628f9ae787786670e3067bf90b053c1a6e13d795`.
The source-built emulator and image identities are recorded in the
[browser release materials](../releasing.md). Both the direct-header and
service-worker hosting paths passed real guest acceptance.
The screenshots are presentation evidence, not a live terminal, a persistence
claim or a replacement for source-bound functional acceptance.

To capture current behavior, run the [browser acceptance driver](../web-browser-checks.md)
against a clean built checkout and review the images in `build/web-browser/`
before copying them here. Keep the source revision and descriptions current.
