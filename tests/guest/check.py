#!/usr/bin/env python3
"""Check console, file and application features in the actual QEMU guest."""
import json
import os
from pathlib import Path
import selectors
import subprocess
import time
import sys

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / "build/image"
sys.path.insert(0, str(ROOT / "scripts"))
import image as identity
import qemu


class Guest:
    def __init__(self):
        self.process = subprocess.Popen(qemu.command(OUT / "emacsos.img", debug_exit=True),
                                        stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                        stderr=subprocess.STDOUT)
        self.selector = selectors.DefaultSelector()
        self.selector.register(self.process.stdout, selectors.EVENT_READ)
        self.transcript = bytearray()

    def pump(self, timeout=0.1):
        for key, _ in self.selector.select(timeout):
            chunk = os.read(key.fd, 65536)
            if chunk:
                self.transcript.extend(chunk)
            else:
                self.selector.unregister(key.fileobj)

    def wait(self, marker, start=0, timeout=45):
        end = time.monotonic() + timeout
        while marker not in self.transcript[start:]:
            self.pump()
            if self.process.poll() is not None and marker not in self.transcript[start:]:
                raise RuntimeError(f"Guest exited before {marker!r}")
            if time.monotonic() >= end:
                raise RuntimeError(f"Guest timeout waiting for {marker!r}")

    def send(self, data):
        for at in range(0, len(data), 128):
            self.process.stdin.write(data[at:at + 128])
            self.process.stdin.flush()
            self.pump(0.005)

    def evaluate(self, form, marker):
        marker = marker.encode()
        # A terminal echo must never be sufficient to pass an assertion.
        if marker in form.encode():
            raise ValueError("Guest result marker occurs literally in typed input")
        start = len(self.transcript)
        self.send(b"\x1b:" + form.encode() + b"\r")
        self.wait(marker, start)

    def close(self):
        if self.process.poll() is None:
            self.process.terminate()
            self.process.wait(timeout=10)
        self.selector.close()
        (OUT / "guest.transcript").write_bytes(self.transcript)


def main():
    manifest = json.loads((OUT / "manifest.json").read_text())
    if manifest["dirty"] or identity.git("status", "--porcelain"):
        raise SystemExit("Guest integration requires a clean committed parent checkout")
    if manifest["parent"] != identity.git("rev-parse", "HEAD"):
        raise SystemExit("Rebuild the image at the current source commit")
    for name, expected in manifest["components"].items():
        if identity.digest(ROOT / name) != expected:
            raise SystemExit(f"Changed build component: {name}")
    checks = []
    guest = Guest()
    result = {"parent": manifest["parent"], "fork": manifest["fork"],
              "image_sha256": manifest["components"]["build/image/emacsos.img"],
              "build_manifest_sha256": identity.digest(OUT / "manifest.json"),
              "scope": "Actual fixed-machine QEMU guest; volatile files, trusted applications",
              "passed": False, "checks": checks}

    def check(name, form, marker):
        guest.evaluate(form, marker)
        checks.append(name)
        print(f"PASS {name}", flush=True)

    try:
        # Console input, Elisp evaluation and command-loop scheduling.
        guest.wait(b"*scratch*")
        checks.append("interactive GNU Emacs startup")
        check("prepare editable buffer", '(progn (switch-to-buffer "*Guest typing*") (erase-buffer) (message (concat "GUEST-" "READY")))', "GUEST-READY")
        guest.send(b"typed in emacsOS")
        check("normal terminal text input", '(progn (unless (equal (buffer-string) "typed in emacsOS") (error "Input differs")) (message (concat "GUEST-" "INPUT-PASS")))', "GUEST-INPUT-PASS")
        check("interactive Elisp evaluation", '(message "GUEST-EVAL-%s" (+ 20 22))', "GUEST-EVAL-42")
        check("command-loop timer", '(progn (run-at-time 0.1 nil (lambda () (message (concat "GUEST-" "TIMER-PASS")))) (message "Timer armed"))', "GUEST-TIMER-PASS")
        # File editing, ordinary saves, errors and source loading.
        source = (ROOT / "tests/guest/counter.el").read_text()
        check("author and save an Elisp application", '(progn (find-file "/work/counter.el") (insert %s) (save-buffer) (setq guest-first-source (buffer-string)) (when (buffer-modified-p) (error "Save left modified state")) (kill-buffer) (message (concat "GUEST-" "SAVED")))' % json.dumps(source), "GUEST-SAVED")
        check("ordinary symlink lock on editing", '(progn (find-file "/work/counter.el") (unless (equal (buffer-string) guest-first-source) (error "Saved source differs")) (goto-char (point-max)) (insert (string 10) ";; Edited inside emacsOS." (string 10)) (unless (file-symlink-p "/work/.#counter.el") (error "No editing lock")) (message (concat "GUEST-" "LOCK-PASS")))', "GUEST-LOCK-PASS")
        check("normal backup and lock cleanup", '(progn (setq guest-second-source (buffer-string)) (save-buffer) (unless (and (not (buffer-modified-p)) (not (file-symlink-p "/work/.#counter.el")) (with-temp-buffer (insert-file-contents "/work/counter.el~") (equal (buffer-string) guest-first-source))) (error "Backup or lock behavior differs")) (kill-buffer) (load-file "/work/counter.el") (unless (with-temp-buffer (insert-file-contents "/work/counter.el") (equal (buffer-string) guest-second-source)) (error "Edited source differs")) (message (concat "GUEST-" "RELOAD-PASS")))', "GUEST-RELOAD-PASS")
        check("standard directory copy rename and delete APIs", '(progn (make-directory "/work/check-dir") (copy-file "/work/counter.el" "/work/check-dir/copy.el") (rename-file "/work/check-dir/copy.el" "/work/check-dir/renamed.el") (unless (and (file-attributes "/work/check-dir/renamed.el") (not (file-exists-p "/work/check-dir/copy.el")) (with-temp-buffer (insert-file-contents "/work/check-dir/renamed.el") (equal (buffer-string) guest-second-source)) (member "renamed.el" (directory-files "/work/check-dir"))) (error "Directory metadata differs")) (delete-file "/work/check-dir/renamed.el") (unless (not (file-exists-p "/work/check-dir/renamed.el")) (error "Deleted file remains")) (delete-directory "/work/check-dir") (when (file-exists-p "/work/check-dir") (error "Deleted directory remains")) (message (concat "GUEST-" "FILES-PASS")))', "GUEST-FILES-PASS")
        check("original readonly save error and modified data", '(progn (with-temp-buffer (insert "unsaved data") (let (observed) (condition-case condition (write-region (point-min) (point-max) "/usr/share/emacs/site-lisp/emacsos-app.el" nil \'silent) (file-error (setq observed condition))) (unless (and observed (string-match-p "Read-only file system" (error-message-string observed)) (buffer-modified-p) (equal (buffer-string) "unsaved data")) (error "Readonly error or buffer state differs")))) (message (concat "GUEST-" "ERROR-PASS")))', "GUEST-ERROR-PASS")
        # Independent application instances and their manager lifecycle.
        check("two independently identified applications", '(progn (setq guest-a (guest-start) guest-b (guest-start)) (unless (and (/= guest-a guest-b) (= 2 (length (emacsos-app-list)))) (error "Instances differ")) (emacsos-processes) (unless (eq major-mode \'emacsos-processes-mode) (error "No process manager")) (message (concat "GUEST-" "APPS-PASS")))', "GUEST-APPS-PASS")
        check("manager source navigation", '(progn (goto-char (or (text-property-any (point-min) (point-max) \'tabulated-list-id guest-a) (error "Missing row"))) (emacsos-processes-source) (unless (equal buffer-file-name "/work/counter.el") (error "Wrong source")) (emacsos-processes) (message (concat "GUEST-" "SOURCE-PASS")))', "GUEST-SOURCE-PASS")
        check("stop and restart through the manager", '(progn (setq guest-a-count (gethash guest-a guest-counts) guest-b-count (gethash guest-b guest-counts)) (with-current-buffer (format "*Guest run %d*" guest-a) (goto-char (point-max)) (insert "user edit")) (goto-char (or (text-property-any (point-min) (point-max) \'tabulated-list-id guest-a) (error "Missing row"))) (emacsos-processes-stop) (unless (eq (plist-get (emacsos-app-inspect guest-a) :state) \'exited) (error "Stop not complete")) (emacsos-processes-restart) (setq guest-new (plist-get (car (last (emacsos-app-list))) :id)) (run-at-time 2.2 nil (lambda () (if (and (/= guest-new guest-a) (> (gethash guest-b guest-counts) guest-b-count) (> (gethash guest-new guest-counts) 1) (= (gethash guest-a guest-counts) guest-a-count) (not (memq (gethash guest-a guest-timers) timer-list)) (with-current-buffer (format "*Guest run %d*" guest-a) (string-match-p "user edit" (buffer-string)))) (message (concat "GUEST-" "LIFECYCLE-PASS")) (error "Lifecycle integration check failed")))) (message "Lifecycle check armed"))', "GUEST-LIFECYCLE-PASS")
        guest.evaluate('(kill-emacs 0)', "Runtime exited status=0x0000000000000000")
        if guest.process.wait(timeout=10) != 33:
            raise RuntimeError("Guest did not exit successfully through the debug device")
        checks.append("clean runtime exit")
        result["passed"] = True
    except Exception as error:
        result["error"] = str(error)
        raise
    finally:
        guest.close()
        result["transcript_sha256"] = identity.digest(OUT / "guest.transcript")
        (OUT / "guest-results.json").write_text(json.dumps(result, indent=2) + "\n")
    print("Guest integration checks passed", flush=True)


if __name__ == "__main__":
    main()
