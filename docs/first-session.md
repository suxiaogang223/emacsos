# Your first five minutes

Open the [project website](https://suxiaogang223.github.io/emacsos/) and choose
**Start emacsOS**, or [run locally](../README.md#run-it-yourself). In the browser
computer, press POWER and wait for **Your workspace is ready.** Click the terminal before typing. The direct
access keys below it send real Emacs keyboard input and return focus to the screen.

`C-` means Control; `M-` means Alt or Option. On a Mac, use Control rather than
Command. If you get stuck in a command, press CANCEL or `C-g`.

## Find your way

The home screen is a read-only Emacs buffer. TAB selects an entry and Enter
opens it. Press `n` there to open the example Org notebook, or `a` for today's
agenda. `C-c o` returns home from any buffer. `C-x b` switches buffers and
`C-x 2` splits a window; the [workspace guide](workspace.md) covers each tool.

## Evaluate a little Lisp

Press EVALUATE (`M-:`), type the expression below and press Enter:

```elisp
(+ 20 22)
```

Emacs displays `42` at the bottom of the screen. You have just evaluated code
inside the Linux guest.

## Make a command of your own

1. Press `C-x C-f`, enter `~/work/hello.el` and press Enter.
2. Type this code into the new file:

   ```elisp
   (defun my-hello ()
     "Say hello from this computer."
     (interactive)
     (message "Hello from your Lisp computer."))
   ```

3. Press `C-x C-s` to save it.
4. Press COMMAND (`M-x`), type `eval-buffer` and press Enter.
5. Press COMMAND again, type `my-hello` and press Enter.

Your function now behaves like any other Emacs command. Change its message,
save, evaluate the buffer again and run it again. No application packaging
step is needed for this experiment.

You can also evaluate the expression immediately before the cursor with
`C-x C-e`, or load a saved file with `M-x load-file`.

## Explore files and the shell

Press FILES (`C-x d`), then Enter. Dired lists your working directory, including
`hello.el`. Move to the file and press Enter to open it.

Press SHELL and wait for Eshell to finish its first load. At the `~/work $`
prompt, enter:

```sh
echo hello
ls
```

Eshell runs inside Emacs. The guest contains a small Linux userspace; additional
external tools need to be added to the image explicitly.

## Leave the session

`C-x C-c` exits Emacs and shuts down the guest. RESTART creates a fresh browser
machine. **Browser files disappear when the page closes, refreshes or restarts.**
The native reference uses a separate retained disk; see
[storage boundaries](design.md#persistence-and-networking).

For the larger picture, read the [architecture](design.md) and
[roadmap](roadmap.md). To help improve the computer, see
[Contributing](../CONTRIBUTING.md).
