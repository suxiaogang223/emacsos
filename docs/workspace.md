# The text workspace

emacsOS keeps Emacs' text interface, keyboard commands, buffers and windows.
The home screen is an ordinary read-only buffer, not a separate desktop or
application manager. Standard `M-x`, `C-x b`, `C-x 2`, `C-x C-f` and `C-h` work
throughout the environment.

## Begin and return

Startup opens `*emacsOS*`. Move between text buttons with TAB and Shift-TAB,
then press Enter. The single-letter shortcuts below apply only in this buffer.
`C-c o` runs `emacsos-home` from anywhere, keeping other buffers and windows.
`M-x emacsos-home` is the equivalent command. Use `C-x b` to switch back.

| Home key | Command | Standard Emacs tool |
| --- | --- | --- |
| `f` | `emacsos-files` | Dired in `~/work` |
| `n` | `emacsos-notes` | An editable Org notebook |
| `a` | `emacsos-agenda` | Today's Org agenda |
| `s` | `emacsos-shell` | Eshell |
| `l` | `emacsos-lisp` | An editable Emacs Lisp example |
| `h` | `info` | The Emacs manual |

The default Org integration enables links to Info manuals. Other standard Org
link modules remain installed and can be enabled through `org-modules`.

These small entry commands are parent system policy in `system/desktop.el`.
They use GNU Emacs directly; extensions can use the same ordinary APIs.

## Write and plan

`M-x emacsos-notes` opens `~/work/notes.org`. TAB folds the current heading,
`M-RET` inserts another heading, and `C-c C-o` follows the link at point.
Edit and save with `C-x C-s`.

`M-x emacsos-tasks` opens `~/work/tasks.org`. Use `C-c C-t` on a task to change
its TODO state and `C-c C-s` to schedule it. Save, then run
`M-x emacsos-agenda` to see today's schedule. In the agenda, Enter visits the
task, `g` refreshes and `q` leaves the agenda.

The first notes, tasks or Lisp entry creates a small example only when its file
is absent and no buffer already visits that path. Examples are saved once;
opening an existing file never replaces its contents or unsaved edits.
Subsequent saves, backups, locks and filesystem errors are ordinary Emacs
behavior. The task example is scheduled for the guest's current date when
first created; later visits do not change its date.

## Make a tool

`M-x emacsos-lisp` opens `~/work/hello.el`. Run `M-x eval-buffer`, then
`M-x my-hello`. Change the message, save and evaluate again.
The result is an ordinary interactive command available through `M-x`.

Use `C-h f` to describe a function, `C-h k` to describe a key and `C-x 2` to
split a window. Use `g` to refresh a Dired listing after changing files elsewhere.
Use Dired and Eshell on the same files; their data does not
belong to separate application containers.

## Lifetime and next steps

In the browser, these files belong to the current machine session. Closing,
refreshing or restarting discards them. Browser persistence, import/export and
guest networking remain future work. Native clean shutdown and retained-disk
reopening have a separate acceptance boundary; see [architecture](design.md).

The [roadmap](roadmap.md) prioritizes workspace continuity next. Keep future
features accessible through normal commands and readable text buffers.
