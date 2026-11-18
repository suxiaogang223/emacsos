# Project screenshots

These are original, unretouched screenshots of the real emacsOS browser
computer, captured by `tests/web/browser.cjs` on November 16, 2026.
They are included under the parent repository's GPL-3.0-or-later license.

| File | Actual state |
| --- | --- |
| `workspace.png` | Linux has booted; GNU Emacs 30.2 displays the welcome workspace |
| `files.png` | Dired displays a file saved and loaded inside the guest |
| `power-on.png` | The computer's powered-off page before boot |

Capture source: `a4c313fd069b067128170898b645f65601a1dbba`, included in
the [Linux migration](https://github.com/suxiaogang223/emacsos/pull/15).
The screenshots are presentation evidence, not a live terminal, a persistence
claim or a replacement for source-bound functional acceptance.

To capture current behavior, run the [browser acceptance driver](../web-browser-checks.md)
against a clean built checkout and review the images in `build/web-browser/`
before copying them here. Keep the source revision and descriptions current.
