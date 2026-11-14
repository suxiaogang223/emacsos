#!/usr/bin/env python3
"""Exercise the built runtime in a host PTY; this is not guest acceptance."""
import argparse
import errno
import fcntl
import hashlib
import json
import os
from pathlib import Path
import pty
import select
import shutil
import subprocess
import tempfile
import termios
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=Path("build/runtime"))
    parser.add_argument("--trace", action="store_true", help="Run under strace when available")
    args = parser.parse_args()
    out = args.output.resolve()
    root = out / "root"
    dumps = [p for p in root.rglob("*.pdmp") if not p.is_symlink()]
    if len(dumps) != 1:
        raise SystemExit("Expected one matching portable dump")
    env = dict(os.environ, TERM="xterm", LC_ALL="C", TZ="UTC")
    env.update({
        "EMACSLOADPATH": str(root / "usr/share/emacs/30.2/lisp"),
        "EMACSDATA": str(root / "usr/share/emacs/30.2/etc"),
        "EMACSDOC": str(root / "usr/share/emacs/30.2/etc"),
        "EMACSPATH": str(dumps[0].parent),
        "TERMINFO": str(root / "usr/share/terminfo"),
    })
    with tempfile.TemporaryDirectory(prefix="emacsos-runtime-smoke-") as work:
        env["HOME"] = work
        destination = Path(work) / "smoke.el"
        # Real terminal input, event-loop timer, Lisp evaluation, write/read and exit.
        form = '''(progn
          (switch-to-buffer "*runtime-smoke*")
          (run-at-time 0.1 0.1
            (lambda ()
              (when (string= (buffer-string) "runtime input")
                (write-region "(+ 20 22)" nil %s nil 'silent)
                (with-temp-buffer
                  (insert-file-contents %s)
                  (unless (= (eval (read (current-buffer))) 42)
                    (error "Runtime file/evaluation smoke failed")))
                (message "EMACSOS-RUNTIME-SMOKE-PASS")
                (redisplay t)
                (kill-emacs 0)))))''' % (json.dumps(str(destination)), json.dumps(str(destination)))
        command = [str(out / "emacs"), "--dump-file", str(dumps[0]), "-Q", "-nw", "--eval", form]
        if args.trace:
            trace = shutil.which("strace")
            if not trace:
                raise SystemExit("--trace requires strace in PATH")
            command = [trace, "-f", "-o", str(out / "host-syscalls.trace"), *command]
        master, slave = pty.openpty()
        process = subprocess.Popen(command, stdin=slave, stdout=slave, stderr=slave, env=env, cwd=work, start_new_session=True, preexec_fn=lambda: fcntl.ioctl(0, termios.TIOCSCTTY, 0))
        os.close(slave)
        transcript = bytearray()
        started = time.monotonic()
        sent = False
        try:
            while time.monotonic() - started < 30:
                if not sent and time.monotonic() - started > 1:
                    os.write(master, b"runtime input")
                    sent = True
                readable, _, _ = select.select([master], [], [], 0.1)
                if readable:
                    try:
                        data = os.read(master, 65536)
                    except OSError as error:
                        if error.errno == errno.EIO:
                            break
                        raise
                    if not data:
                        break
                    transcript.extend(data)
                if process.poll() is not None and not readable:
                    break
            if process.poll() is None:
                process.kill()
                process.wait()
                raise RuntimeError("Host PTY smoke timed out")
            if process.returncode != 0 or b"EMACSOS-RUNTIME-SMOKE-PASS" not in transcript:
                raise RuntimeError(f"Host PTY smoke failed: {process.returncode}")
            if destination.read_text() != "(+ 20 22)":
                raise RuntimeError("Host smoke file contents differ")
        finally:
            os.close(master)
            if process.poll() is None:
                process.kill()
                process.wait()
            (out / "host-smoke.transcript").write_bytes(transcript)
        (out / "host-smoke.json").write_text(json.dumps({
            "scope": "host PTY only; no guest execution",
            "elf_sha256": hashlib.sha256((out / "emacs").read_bytes()).hexdigest(),
            "dump_sha256": hashlib.sha256(dumps[0].read_bytes()).hexdigest(),
            "passed": True,
            "checks": ["terminal input", "command loop timer", "Elisp evaluation", "file write/read"],
            "trace": args.trace,
        }, indent=2) + "\n")
        print("Host runtime PTY smoke passed")


if __name__ == "__main__":
    main()
