#!/usr/bin/env python3
"""Check console, file and application features in the actual QEMU guest."""
import argparse
import json
import os
from pathlib import Path
import selectors
import shutil
import subprocess
import time
import sys

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / "build/image"
sys.path.insert(0, str(ROOT / "scripts"))
import image as identity
import qemu


class Guest:
    def __init__(self, *, data=None, control=None, output=OUT, label="guest"):
        self.output, self.label = output, label
        output.mkdir(parents=True, exist_ok=True)
        self.process = subprocess.Popen(qemu.command(OUT / "emacsos.img", debug_exit=True,
                                                      data=data, control=control),
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
        (self.output / f"{self.label}.transcript").write_bytes(self.transcript)


def validated_manifest():
    manifest = json.loads((OUT / "manifest.json").read_text())
    if manifest["dirty"] or identity.git("status", "--porcelain"):
        raise SystemExit("Guest integration requires a clean committed parent checkout")
    if manifest["parent"] != identity.git("rev-parse", "HEAD"):
        raise SystemExit("Rebuild the image at the current source commit")
    for name, expected in manifest["components"].items():
        if identity.digest(ROOT / name) != expected:
            raise SystemExit(f"Changed build component: {name}")
    return manifest


def main(data=None, output=OUT, label="guest"):
    manifest = validated_manifest()
    if data is None:
        data = output / f"{label}-data.img"
        data.unlink(missing_ok=True)
        qemu.prepare_data(data, OUT / "data-empty.img")
    checks = []
    guest = Guest(data=data, output=output, label=label)
    result = {"parent": manifest["parent"], "fork": manifest["fork"],
              "image_sha256": manifest["components"]["build/image/emacsos.img"],
              "build_manifest_sha256": identity.digest(OUT / "manifest.json"),
              "scope": "Actual fixed-machine QEMU guest; trusted applications",
              "data_disk": str(data) if data else None,
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
        result["transcript_sha256"] = identity.digest(output / f"{label}.transcript")
        (output / f"{label}-results.json").write_text(json.dumps(result, indent=2) + "\n")
    print("Guest integration checks passed", flush=True)


class Control:
    """Use a separate UART pipe, never terminal output, for storage barriers."""
    def __init__(self, directory):
        self.path = directory / "control"
        self.session = "0000000000000001"
        self.request = 0
        self.pending = bytearray()
        self.paths = [Path(str(self.path) + suffix) for suffix in (".in", ".out")]
        for path in self.paths:
            path.unlink(missing_ok=True)
            os.mkfifo(path)
        self.input, self.output = [os.open(path, os.O_RDWR | os.O_NONBLOCK) for path in self.paths]

    def call(self, guest, action):
        self.request += 1
        request = f"{self.request:016x}"
        os.write(self.input, f"EOS1 {self.session} {request} {action}\n".encode())
        end = time.monotonic() + 30
        while time.monotonic() < end:
            guest.pump(0.01)
            try:
                self.pending.extend(os.read(self.output, 4096))
            except BlockingIOError:
                pass
            while b"\n" in self.pending:
                line, _, rest = self.pending.partition(b"\n")
                self.pending = bytearray(rest)
                fields = line.decode().split()
                if len(fields) == 8 and fields[:3] == ["EOS1", self.session, request]:
                    result = {"state": fields[3], "generation": int(fields[4], 16),
                              "errno": int(fields[5]), "mounted": fields[6] == "1",
                              "frozen": fields[7] == "1"}
                    if result["errno"]:
                        raise RuntimeError(f"Storage {action} failed: {result}")
                    return result
            if guest.process.poll() is not None:
                raise RuntimeError("Guest exited before storage acknowledgement")
        raise RuntimeError(f"Storage {action} acknowledgement timed out")

    def close(self):
        for descriptor in (self.input, self.output):
            os.close(descriptor)
        for path in self.paths:
            path.unlink(missing_ok=True)


def storage_main():
    manifest = validated_manifest()
    output = OUT / "storage"
    output.mkdir(exist_ok=True)
    disk, snapshot = output / "data.img", output / "checkpoint.img"
    disk.unlink(missing_ok=True)
    snapshot.unlink(missing_ok=True)
    qemu.prepare_data(disk, OUT / "data-empty.img")
    system_hash = identity.digest(OUT / "emacsos.img")
    checks = []
    record = {"parent": manifest["parent"], "fork": manifest["fork"],
              "system_sha256": system_hash, "passed": False, "checks": checks}
    channel = Control(output)
    guest = Guest(data=disk, control=channel.path, output=output, label="write")
    try:
        guest.wait(b"*scratch*")
        status = channel.call(guest, "STATUS")
        if not status["mounted"] or status["frozen"]:
            raise RuntimeError(f"DATA not mounted normally: {status}")
        guest.evaluate('(progn (unless (null write-region-inhibit-fsync) (error "Save sync disabled")) (unless (and (file-directory-p "/work") (file-directory-p "/home/user") (file-directory-p "/tmp")) (error "Missing DATA directories")) (message (concat "STORE-" "MOUNTED")))', "STORE-MOUNTED")
        checks.append("fresh guest mounts DATA directories and enables normal save sync")
        source = '(defun retained-computation () (+ 19 23))\n(provide \'retained)\n'
        guest.evaluate('(progn (find-file "/work/retained.el") (erase-buffer) (insert %s) (save-buffer) (set-file-modes buffer-file-name 416) (kill-buffer) (make-symbolic-link "retained.el" "/work/source-link.el") (with-temp-file "/work/.#user-data" (insert "ordinary user filename")) (with-temp-file "/work/stale.el" (insert "previous content")) (make-symbolic-link (format "%%s@%%s.%%d" (user-login-name) (system-name) (emacs-pid)) "/work/.#stale.el") (message (concat "STORE-" "SAVED")))' % json.dumps(source), "STORE-SAVED")
        checks.append("save source, metadata, links and a real prior-boot lock")
        guest.evaluate('(progn (find-file "/work/retained.el") (setq-local file-precious-flag t) (goto-char (point-max)) (insert ";; Atomic replacement through normal Emacs saving." (string 10)) (save-buffer) (when (buffer-modified-p) (error "Precious save remained modified")) (kill-buffer) (message (concat "STORE-" "PRECIOUS")))', "STORE-PRECIOUS")
        checks.append("normal precious-file save uses atomic replacement and preserves metadata")
        guest.evaluate('(progn (with-temp-file "/home/user/retained-note.txt" (insert "home is on DATA")) (with-temp-file "/work/cross-directory.txt" (insert "one filesystem")) (setq moved-inode (file-attribute-inode-number (file-attributes "/work/cross-directory.txt"))) (rename-file "/work/cross-directory.txt" "/home/user/cross-directory.txt") (unless (= moved-inode (file-attribute-inode-number (file-attributes "/home/user/cross-directory.txt"))) (error "Home rename changed identity")) (rename-file "/home/user/cross-directory.txt" "/tmp/cross-directory.txt") (unless (= moved-inode (file-attribute-inode-number (file-attributes "/tmp/cross-directory.txt"))) (error "Temporary-directory rename changed identity")) (message (concat "STORE-" "ONE-VOLUME")))', "STORE-ONE-VOLUME")
        checks.append("home and temporary files use the same DATA volume with stable cross-directory rename identity")
        frozen = channel.call(guest, "FREEZE")
        if frozen["state"] != "FROZEN" or not frozen["frozen"]:
            raise RuntimeError(f"No frozen checkpoint: {frozen}")
        guest.evaluate('(progn (with-temp-buffer (insert "unsaved") (let (observed) (condition-case condition (write-region (point-min) (point-max) "/work/frozen.txt") (file-error (setq observed condition))) (unless (and observed (buffer-modified-p)) (error "Frozen writes accepted")))) (message (concat "STORE-" "FROZEN")))', "STORE-FROZEN")
        guest.evaluate('(progn (dolist (directory (list "/work/" "/home/user/" "/tmp/")) (with-temp-buffer (insert "frozen data") (let (observed) (condition-case condition (write-region (point-min) (point-max) (concat directory "freeze-denied.txt")) (file-error (setq observed condition))) (unless (and observed (buffer-modified-p)) (error "DATA directory escaped freeze"))))) (message (concat "STORE-" "ALL-FROZEN")))', "STORE-ALL-FROZEN")
        shutil.copyfile(disk, snapshot)
        checkpoint_hash = identity.digest(snapshot)
        checks.append("acknowledged freeze rejects writes and yields a consistent disk copy")
        thawed = channel.call(guest, "THAW")
        if thawed["frozen"]:
            raise RuntimeError("DATA did not thaw")
        guest.evaluate('(progn (with-temp-file "/work/after-copy.txt" (insert "later generation")) (message (concat "STORE-" "THAWED")))', "STORE-THAWED")
        if identity.digest(snapshot) != checkpoint_hash:
            raise RuntimeError("Checkpoint copy changed after guest resumed")
        checks.append("thaw resumes writes without changing the copied generation")
        guest.evaluate('(kill-emacs 0)', "Runtime exited status=0x0000000000000000")
        if guest.process.wait(timeout=10) != 33:
            raise RuntimeError("Writer guest exit failed")
    except Exception as error:
        record["error"] = str(error)
        raise
    finally:
        guest.close()
        channel.close()
        (output / "results.json").write_text(json.dumps(record, indent=2) + "\n")
    guest = Guest(data=snapshot, output=output, label="restore")
    try:
        guest.wait(b"*scratch*")
        guest.evaluate('(progn (load-file "/work/retained.el") (unless (and (= 42 (retained-computation)) (= 416 (file-modes "/work/retained.el")) (equal (file-symlink-p "/work/source-link.el") "retained.el") (not (file-exists-p "/work/after-copy.txt")) (with-temp-buffer (insert-file-contents "/work/.#user-data") (equal (buffer-string) "ordinary user filename"))) (error "Retained data differs")) (message (concat "STORE-" "RESTORED")))', "STORE-RESTORED")
        checks.append("independent reboot restores checkpoint source, modes and symlinks")
        guest.evaluate('(progn (unless (and (with-temp-buffer (insert-file-contents "/home/user/retained-note.txt") (equal (buffer-string) "home is on DATA")) (with-temp-buffer (insert-file-contents "/tmp/cross-directory.txt") (equal (buffer-string) "one filesystem")) (not (file-exists-p "/work/cross-directory.txt")) (not (file-exists-p "/home/user/cross-directory.txt"))) (error "DATA home or temporary files changed")) (message (concat "STORE-" "DIRECTORIES-RESTORED")))', "STORE-DIRECTORIES-RESTORED")
        checks.append("home and ordinary temporary-directory contents survive independent reboot")
        guest.evaluate('(progn (setq prior-lock (file-symlink-p "/work/.#stale.el")) (find-file "/work/stale.el") (goto-char (point-max)) (insert "edited") (unless (and (file-symlink-p "/work/.#stale.el") (not (equal prior-lock (file-symlink-p "/work/.#stale.el")))) (error "Previous boot lock was reused")) (save-buffer) (kill-buffer) (message (concat "STORE-" "LOCK-RECOVERED")))', "STORE-LOCK-RECOVERED")
        checks.append("normal Emacs locking replaces stale prior-boot owner safely")
        guest.evaluate('(kill-emacs 0)', "Runtime exited status=0x0000000000000000")
        if guest.process.wait(timeout=10) != 33:
            raise RuntimeError("Restored guest exit failed")
        if identity.digest(OUT / "emacsos.img") != system_hash:
            raise RuntimeError("Storage operations changed SYSTEM image")
        record.update(passed=True, checkpoint_sha256=checkpoint_hash,
                      data_sha256=identity.digest(disk), restored_sha256=identity.digest(snapshot))
        checks.append("SYSTEM image remains unchanged")
    except Exception as error:
        record["error"] = str(error)
        raise
    finally:
        guest.close()
        (output / "results.json").write_text(json.dumps(record, indent=2) + "\n")
    print(f"Storage guest checks passed: {len(checks)}", flush=True)


def recovery_main():
    """Absent or rejected DATA must never silently enable writable RAM files."""
    manifest = validated_manifest()
    output = OUT / "recovery"
    output.mkdir(exist_ok=True)
    profiles = []
    record = {"parent": manifest["parent"], "fork": manifest["fork"],
              "image_sha256": manifest["components"]["build/image/emacsos.img"],
              "passed": False, "profiles": profiles}
    try:
        for name, size in (("absent", None), ("corrupt", qemu.DATA_SIZE),
                           ("unsupported-size", qemu.DATA_SIZE // 2)):
            data = None
            if size is not None:
                data = output / f"{name}.img"
                with data.open("wb") as stream:
                    stream.truncate(size)
            before = identity.digest(data) if data else None
            guest = Guest(data=data, output=output, label=name)
            try:
                guest.wait(b"DATA unavailable: read-only recovery")
                guest.wait(b"*scratch*")
                guest.evaluate('(progn (switch-to-buffer "*Recovery editing*") (insert "recoverable buffer") (unless (and (= (+ 20 22) 42) (file-readable-p "/usr/share/emacs/site-lisp/emacsos-app.el")) (error "Recovery environment unavailable")) (dolist (directory (list "/work/" "/home/user/" "/tmp/")) (with-temp-buffer (insert "unsaved recovery data") (let (observed) (condition-case condition (write-region (point-min) (point-max) (concat directory "must-not-be-volatile.txt")) (file-error (setq observed condition))) (unless (and observed (string-match-p "Read-only file system" (error-message-string observed)) (buffer-modified-p)) (error "Writable fallback or incorrect recovery error"))))) (message (concat "RECOVERY-" "PASS")))', "RECOVERY-PASS")
                guest.evaluate('(kill-emacs 0)', "Runtime exited status=0x0000000000000000")
                if guest.process.wait(timeout=10) != 33:
                    raise RuntimeError(f"{name} recovery exit failed")
            finally:
                guest.close()
            if data and identity.digest(data) != before:
                raise RuntimeError(f"{name} recovery modified rejected DATA")
            profiles.append({"name": name, "passed": True, "data_sha256": before,
                             "transcript_sha256": identity.digest(output / f"{name}.transcript")})
            print(f"PASS {name}: interactive read-only recovery without writable fallback", flush=True)
        record["passed"] = True
    except Exception as error:
        record["error"] = str(error)
        raise
    finally:
        (output / "results.json").write_text(json.dumps(record, indent=2) + "\n")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--storage", action="store_true", help="Check DATA across separate guest launches")
    args = parser.parse_args()
    if args.storage:
        storage_main()
        recovery_main()
    else:
        main()
