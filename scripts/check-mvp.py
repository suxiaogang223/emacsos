#!/usr/bin/env python3
"""Accept the MVP through actual GNU Emacs commands over QEMU's serial console."""
import json
import os
from pathlib import Path
import selectors
import subprocess
import time

from importlib.util import module_from_spec, spec_from_file_location

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "build/mvp"
spec = spec_from_file_location("mvp_manifest", ROOT / "scripts/mvp-manifest.py")
identity = module_from_spec(spec)
spec.loader.exec_module(identity)


class Guest:
    def __init__(self):
        self.process = subprocess.Popen([
            "qemu-system-x86_64", "-M", "pc", "-m", "1G", "-nic", "none",
            "-display", "none", "-monitor", "none", "-serial", "stdio", "-no-reboot",
            "-device", "isa-debug-exit,iobase=0xf4,iosize=0x04", "-drive",
            f"file={OUT / 'emacsos.img'},format=raw,if=ide,snapshot=on"],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
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
            raise ValueError("Acceptance marker occurs literally in typed input")
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
        raise SystemExit("MVP acceptance requires a clean committed parent checkout")
    if manifest["parent"] != identity.git("rev-parse", "HEAD"):
        raise SystemExit("Rebuild the image at the current source commit")
    for name, expected in manifest["components"].items():
        if identity.digest(ROOT / name) != expected:
            raise SystemExit(f"Changed build component: {name}")
    checks = []
    guest = Guest()
    result = {"parent": manifest["parent"], "fork": manifest["fork"],
              "image_sha256": manifest["components"]["build/mvp/emacsos.img"],
              "build_manifest_sha256": identity.digest(OUT / "manifest.json"),
              "scope": "Actual fixed-machine QEMU guest; volatile files, trusted applications",
              "passed": False, "checks": checks}

    def check(name, form, marker):
        guest.evaluate(form, marker)
        checks.append(name)
        print(f"PASS {name}", flush=True)

    try:
        guest.wait(b"*scratch*")
        checks.append("interactive GNU Emacs startup")
        check("prepare editable buffer", '(progn (switch-to-buffer "*MVP typing*") (erase-buffer) (message (concat "MVP-" "READY")))', "MVP-READY")
        guest.send(b"typed in emacsOS")
        check("normal terminal text input", '(progn (unless (equal (buffer-string) "typed in emacsOS") (error "Input differs")) (message (concat "MVP-" "INPUT-PASS")))', "MVP-INPUT-PASS")
        check("interactive Elisp evaluation", '(message "MVP-EVAL-%s" (+ 20 22))', "MVP-EVAL-42")
        check("command-loop timer", '(progn (run-at-time 0.1 nil (lambda () (message (concat "MVP-" "TIMER-PASS")))) (message "Timer armed"))', "MVP-TIMER-PASS")
        source = (ROOT / "tests/guest/mvp-program.el").read_text()
        check("author and save an Elisp application", '(progn (find-file "/work/mvp-program.el") (insert %s) (save-buffer) (setq mvp-first-source (buffer-string)) (when (buffer-modified-p) (error "Save left modified state")) (kill-buffer) (message (concat "MVP-" "SAVED")))' % json.dumps(source), "MVP-SAVED")
        check("ordinary symlink lock on editing", '(progn (find-file "/work/mvp-program.el") (goto-char (point-max)) (insert (string 10) ";; Edited inside emacsOS." (string 10)) (unless (file-symlink-p "/work/.#mvp-program.el") (error "No editing lock")) (message (concat "MVP-" "LOCK-PASS")))', "MVP-LOCK-PASS")
        check("normal backup and lock cleanup", '(progn (save-buffer) (unless (and (not (buffer-modified-p)) (not (file-symlink-p "/work/.#mvp-program.el")) (with-temp-buffer (insert-file-contents "/work/mvp-program.el~") (equal (buffer-string) mvp-first-source))) (error "Backup or lock behavior differs")) (kill-buffer) (load-file "/work/mvp-program.el") (message (concat "MVP-" "RELOAD-PASS")))', "MVP-RELOAD-PASS")
        check("standard directory copy rename and delete APIs", '(progn (make-directory "/work/check-dir") (copy-file "/work/mvp-program.el" "/work/check-dir/copy.el") (rename-file "/work/check-dir/copy.el" "/work/check-dir/renamed.el") (unless (and (file-attributes "/work/check-dir/renamed.el") (member "renamed.el" (directory-files "/work/check-dir"))) (error "Directory metadata differs")) (delete-file "/work/check-dir/renamed.el") (delete-directory "/work/check-dir") (message (concat "MVP-" "FILES-PASS")))', "MVP-FILES-PASS")
        check("original readonly save error and modified data", '(progn (with-temp-buffer (insert "unsaved data") (let (observed) (condition-case condition (write-region (point-min) (point-max) "/usr/share/emacs/site-lisp/emacsos-app.el" nil \'silent) (file-error (setq observed condition))) (unless (and observed (string-match-p "Read-only file system" (error-message-string observed)) (buffer-modified-p) (equal (buffer-string) "unsaved data")) (error "Readonly error or buffer state differs")))) (message (concat "MVP-" "ERROR-PASS")))', "MVP-ERROR-PASS")
        check("two independently identified applications", '(progn (setq mvp-a (mvp-start) mvp-b (mvp-start)) (unless (and (/= mvp-a mvp-b) (= 2 (length (emacsos-app-list)))) (error "Instances differ")) (emacsos-processes) (unless (eq major-mode \'emacsos-processes-mode) (error "No process manager")) (message (concat "MVP-" "APPS-PASS")))', "MVP-APPS-PASS")
        check("manager source navigation", '(progn (goto-char (or (text-property-any (point-min) (point-max) \'tabulated-list-id mvp-a) (error "Missing row"))) (emacsos-processes-source) (unless (equal buffer-file-name "/work/mvp-program.el") (error "Wrong source")) (emacsos-processes) (message (concat "MVP-" "SOURCE-PASS")))', "MVP-SOURCE-PASS")
        check("stop and restart through the manager", '(progn (setq mvp-a-count (gethash mvp-a mvp-counts) mvp-b-count (gethash mvp-b mvp-counts)) (with-current-buffer (format "*MVP run %d*" mvp-a) (goto-char (point-max)) (insert "user edit")) (goto-char (or (text-property-any (point-min) (point-max) \'tabulated-list-id mvp-a) (error "Missing row"))) (emacsos-processes-stop) (unless (eq (plist-get (emacsos-app-inspect mvp-a) :state) \'exited) (error "Stop not complete")) (emacsos-processes-restart) (setq mvp-new (plist-get (car (last (emacsos-app-list))) :id)) (run-at-time 2.2 nil (lambda () (if (and (/= mvp-new mvp-a) (> (gethash mvp-b mvp-counts) mvp-b-count) (> (gethash mvp-new mvp-counts) 1) (= (gethash mvp-a mvp-counts) mvp-a-count) (not (memq (gethash mvp-a mvp-timers) timer-list)) (with-current-buffer (format "*MVP run %d*" mvp-a) (string-match-p "user edit" (buffer-string)))) (message (concat "MVP-" "LIFECYCLE-PASS")) (error "Lifecycle acceptance failed")))) (message "Lifecycle check armed"))', "MVP-LIFECYCLE-PASS")
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
        (OUT / "acceptance.json").write_text(json.dumps(result, indent=2) + "\n")
    print("MVP guest acceptance passed", flush=True)


if __name__ == "__main__":
    main()
