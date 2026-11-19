# Project screenshots

These are original, unretouched screenshots of the real emacsOS browser
computer, captured on November 19, 2026. They are included under the parent
repository's GPL-3.0-or-later license.

| File | Actual state |
| --- | --- |
| `home.png` | The read-only home buffer, with keyboard-selectable text entries |
| `notes.png` | An editable Org notebook in the Linux guest |
| `agenda.png` | Today's Org agenda, reading the scheduled task from `tasks.org` |
| `files.png` | Dired after saving and loading `experiment.el` in the guest |
| `shell.png` | Eshell listing the workspace and evaluating a Lisp expression |
| `lisp.png` | The editable `hello.el` example defining `my-hello` |
| `website.png` | The project homepage displaying these actual terminal captures |

Application captures come from `tests/web/browser.cjs` at clean Web and guest
source `2f14b0ae9f7880c643b7e09cc8d9b91610acf367`. All 25 browser checks passed with
actual downloaded image hashes matched to the manifest. Emacs remains pinned
to `73cefc0fc8dad3e6709c2d1070b6cfe93102b3a5`.
The terminal images crop the real xterm display. Standard Emacs window and
movement commands prepare each view; no application display is synthesized.
The homepage preview uses the same HTML/CSS revision with these gallery assets.

Actual guest artifact SHA-256 values:

```text
vmlinuz  77ace6bdf0ddff5473fadfb61a2bbcc8ae8dc04ec08a14548ea152fa24cc3612
initramfs.cpio.gz  53252ec1dc2ce906a4e6ac3e428316f2556521313bfa45458362b7af9f4496a2
runtime.squashfs  b6124116511aa7e5820f6ac9ff1054635b2130a415ce7c8253161ab33108c90b
data-template.ext4  a7a98deee7200f13b00a6804facba53530fe9beec9c1aff78241e83cf3eb71ad
```

To capture current behavior, run the [browser acceptance driver](../web-browser-checks.md)
against a clean built checkout. Review its six terminal images before copying
them here, then capture the homepage with `tests/web/site.cjs`. Keep the source
revision, hashes and descriptions current. Runtime source and release identities
are described in [release materials](../releasing.md).

Screenshots are presentation evidence. They do not establish browser retention,
networking or a deployed release. Native and browser acceptance remain separate.
