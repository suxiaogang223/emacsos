#!/usr/bin/env python3
"""Accept the actual Linux guest through its interactive serial command loop."""
import argparse
import contextlib
import http.server
import json
import os
from pathlib import Path
import secrets
import selectors
import shutil
import ssl
import subprocess
import sys
import tempfile
import threading
import time
from urllib.parse import urlsplit

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
from machine import OUT, command, create_disk, digest, git, run, verify, write_json


class Guest:
    def __init__(self, disk, transcript):
        self.path = transcript
        self.transcript = bytearray()
        self.started = time.monotonic()
        self.process = subprocess.Popen(command(disk), stdin=subprocess.PIPE,
                                        stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        self.selector = selectors.DefaultSelector()
        self.selector.register(self.process.stdout, selectors.EVENT_READ)

    def pump(self, timeout=0.1):
        for key, _ in self.selector.select(timeout):
            data = os.read(key.fd, 65536)
            if data:
                self.transcript.extend(data)
                self.path.write_bytes(self.transcript)
            else:
                self.selector.unregister(key.fileobj)

    def wait(self, marker, start=0, timeout=60):
        end = time.monotonic() + timeout
        while marker not in self.transcript[start:]:
            self.pump()
            if b"LINUX-CHECK-ERROR" in self.transcript[start:]:
                raise RuntimeError(f"Interactive Elisp failed; see {self.path}")
            if self.process.poll() is not None:
                raise RuntimeError(f"Guest exited before {marker!r}; see {self.path}")
            if time.monotonic() >= end:
                raise RuntimeError(f"Guest timeout waiting for {marker!r}; see {self.path}")

    def send(self, data):
        for offset in range(0, len(data), 128):
            self.process.stdin.write(data[offset:offset + 128])
            self.process.stdin.flush()
            self.pump(0.005)

    def evaluate(self, form, marker):
        marker = marker.encode()
        if marker in form.encode():
            raise ValueError("Result marker occurs in input; terminal echo could pass the check")
        start = len(self.transcript)
        guarded = ('(condition-case failure %s (error (send-string-to-terminal '
                   '(format "\\nLINUX-%%s %%S\\n" "CHECK-ERROR" failure))))') % form
        self.send(b"\x1b:" + guarded.encode() + b"\r")
        self.wait(marker, start)

    def shutdown(self):
        self.evaluate("(kill-emacs 0)", "EMACSOS-LINUX-EXIT status=0")
        end = time.monotonic() + 30
        while self.process.poll() is None and time.monotonic() < end:
            self.pump()
        self.pump(0)
        if self.process.poll() != 0 or b"EMACSOS-LINUX-UNMOUNT-FAILED" in self.transcript:
            raise RuntimeError("Guest did not cleanly unmount and power off")

    def close(self):
        if self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=5)
        self.pump(0)
        self.selector.close()
        self.process.stdin.close()
        self.process.stdout.close()
        self.path.write_bytes(self.transcript)


@contextlib.contextmanager
def https_fixture():
    """Serve an unpredictable response with a test-only CA and IP certificate."""
    with tempfile.TemporaryDirectory(prefix=".tls-", dir=OUT) as temporary:
        directory = Path(temporary)
        certificate, key = directory / "ca.pem", directory / "key.pem"
        run("openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1",
            "-keyout", key, "-out", certificate, "-subj", "/CN=emacsOS Linux test CA",
            "-addext", "basicConstraints=critical,CA:TRUE", stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL)
        server_key = directory / "server-key.pem"
        request = directory / "server.csr"
        server_certificate = directory / "server.pem"
        extensions = directory / "extensions.cnf"
        extensions.write_text("subjectAltName=IP:10.0.2.2\nbasicConstraints=critical,CA:FALSE\n"
                              "keyUsage=critical,digitalSignature,keyEncipherment\n"
                              "extendedKeyUsage=serverAuth\n")
        run("openssl", "req", "-new", "-newkey", "rsa:2048", "-nodes", "-keyout", server_key,
            "-out", request, "-subj", "/CN=emacsOS Linux test server",
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        run("openssl", "x509", "-req", "-in", request, "-CA", certificate, "-CAkey", key,
            "-CAcreateserial", "-days", "1", "-extfile", extensions, "-out", server_certificate,
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        nonce = secrets.token_hex(24)

        class Handler(http.server.BaseHTTPRequestHandler):
            def handle(self):
                # Certificate rejection intentionally closes before HTTP input.
                try:
                    super().handle()
                except (ConnectionError, ssl.SSLError):
                    pass

            def do_GET(self):
                payload = nonce.encode()
                self.send_response(200)
                self.send_header("Content-Length", str(len(payload)))
                self.end_headers()
                self.wfile.write(payload)

            def log_message(self, *args):
                pass

        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        server.daemon_threads = True
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(server_certificate, server_key)
        server.socket = context.wrap_socket(server.socket, server_side=True)
        worker = threading.Thread(target=server.serve_forever, daemon=True)
        worker.start()
        try:
            yield certificate, f"https://10.0.2.2:{server.server_port}/", nonce
        finally:
            server.shutdown()
            server.server_close()
            worker.join(timeout=5)


def check(internet_url=None):
    manifest = verify()
    if manifest["dirty"] or git("status", "--porcelain"):
        raise RuntimeError("Commit the source before recording guest acceptance")
    if manifest["parent"] != git("rev-parse", "HEAD"):
        raise RuntimeError("Rebuild at the current source commit before acceptance")
    checks, guests = [], []
    result = {"passed": False, "parent": manifest["parent"], "checks": checks,
              "manifest_sha256": digest(OUT / "manifest.json"),
              "scope": "Native QEMU Linux guest; browser capabilities need separate acceptance",
              "internet_url": internet_url, "boot_seconds": [],
              "network_scope": "Synchronous HTTPS only; asynchronous HTTPS remains unresolved"}

    def assertion(guest, name, condition):
        index = len(checks)
        form = (f'(progn (unless {condition} (error "Check {index} failed")) '
                f'(message "LINUX-PASS-%d" {index}))')
        guest.evaluate(form, f"LINUX-PASS-{index}")
        checks.append(name)
        print(f"PASS {name}", flush=True)

    disk = OUT / "acceptance-data.ext4"
    # Only this generated test disk is replaced. data.ext4 is never touched.
    disk.unlink(missing_ok=True)
    for name in ("first-boot.transcript", "second-boot.transcript", "acceptance.json"):
        (OUT / name).unlink(missing_ok=True)
    try:
        with https_fixture() as (certificate, url, nonce):
            create_disk(disk, certificate)
            guest = Guest(disk, OUT / "first-boot.transcript")
            guests.append(guest)
            guest.wait(b"Your workspace is ready.")
            result["boot_seconds"].append(round(time.monotonic() - guest.started, 3))
            assertion(guest, "interactive Linux Emacs as uid 1000",
                      "(and (eq system-type 'gnu/linux) (= (user-uid) 1000) (equal emacs-version \"30.2\"))")
            guest.evaluate('(progn (switch-to-buffer "*typing*") (erase-buffer) (message "LINUX-%s" "TYPE"))', "LINUX-TYPE")
            guest.send(b"typed inside Linux Emacs")
            assertion(guest, "normal terminal text input", '(equal (buffer-string) "typed inside Linux Emacs")')
            source = f';;; Probe authored inside the guest.\n(setq linux-probe "{nonce}")\n'
            guest.evaluate('(progn (find-file "/home/emacs/work/probe.el") (insert %s) (save-buffer) (message "LINUX-%%s" "SAVED"))' % json.dumps(source), "LINUX-SAVED")
            assertion(guest, "ordinary Emacs save and cleared modified state", '(and (not (buffer-modified-p)) (file-exists-p buffer-file-name))')
            guest.evaluate('(progn (goto-char (point-max)) (insert %s) (message "LINUX-%%s" "EDITED"))' % json.dumps(";; second revision\n"), "LINUX-EDITED")
            assertion(guest, "editing creates normal file lock", '(file-symlink-p "/home/emacs/work/.#probe.el")')
            guest.evaluate('(progn (save-buffer) (message "LINUX-%s" "RESAVED"))', "LINUX-RESAVED")
            assertion(guest, "backup preserves original source and save releases lock",
                      '(and (not (file-symlink-p "/home/emacs/work/.#probe.el")) (with-temp-buffer (insert-file-contents "/home/emacs/work/probe.el~") (equal (buffer-string) %s)))' % json.dumps(source))
            assertion(guest, "saved Elisp executes", f'(progn (load-file "/home/emacs/work/probe.el") (equal linux-probe "{nonce}"))')
            guest.evaluate('(progn (setq linux-ticks 0 linux-worker (make-process :name "worker" :buffer "*worker*" :command \'("/bin/sh" "-c" "while :; do sleep 1; done") :noquery t) linux-timer (run-at-time 0 0.1 (lambda () (setq linux-ticks (1+ linux-ticks))))) (message "LINUX-%s" "WORKER"))', "LINUX-WORKER")
            assertion(guest, "timer and command loop progress beside a running worker", '(and (> linux-ticks 0) (process-live-p linux-worker))')
            guest.evaluate('(progn (kill-process linux-worker) (message "LINUX-%s" "KILL-SENT"))', "LINUX-KILL-SENT")
            assertion(guest, "worker can be forcibly stopped", '(and (not (process-live-p linux-worker)) (eq (process-status linux-worker) \'signal))')
            guest.evaluate('(progn (setq linux-fault (make-process :name "fault" :buffer "*fault*" :command \'("/bin/sh" "-c" "kill -SEGV $$") :noquery t)) (message "LINUX-%s" "FAULT-SENT"))', "LINUX-FAULT-SENT")
            assertion(guest, "worker segmentation fault leaves Emacs alive", "(and (eq (process-status linux-fault) 'signal) (= (process-exit-status linux-fault) 11) (> linux-ticks 1))")
            assertion(guest, "GnuTLS available", "(progn (require 'url) (require 'gnutls) (require 'nsm) (advice-add 'nsm-query :before (lambda (&rest arguments) (send-string-to-terminal (format \"\\nTLS policy: %s\\n\" (car (last arguments)))))) (gnutls-available-p))")
            # Configure this disposable acceptance session with a test-only CA,
            # then restore the distribution CA bundle for the public request.
            request = ('(progn (setq linux-system-trustfiles gnutls-trustfiles '
                       'gnutls-trustfiles (list "/home/emacs/work/fixture-ca.pem") '
                       'gnutls-verify-error t network-security-level \'high nsm-noninteractive t url-request-noninteractive t url-proxy-services nil) '
                       '(let ((response (url-retrieve-synchronously %s t t))) '
                       '(unless response (error "No HTTPS response")) '
                       '(unwind-protect (with-current-buffer response (goto-char (point-min)) '
                       '(and (= url-http-response-status 200) (search-forward %s nil t))) (kill-buffer response))))')
            assertion(guest, "HTTPS with certificate verification and unpredictable response",
                      request % (json.dumps(url), json.dumps(nonce)))
            assertion(guest, "untrusted HTTPS certificate rejected",
                      '(progn (setq gnutls-trustfiles nil) (let ((kill-buffer-query-functions nil)) (condition-case failure (progn (open-network-stream "untrusted" nil "10.0.2.2" %d :type \'tls) nil) (error (string-match-p "Certificate validation failed" (error-message-string failure))))))' % urlsplit(url).port)
            guest.evaluate('(progn (setq gnutls-trustfiles linux-system-trustfiles) (message "LINUX-%s" "TRUST-RESTORED"))', "LINUX-TRUST-RESTORED")
            if internet_url:
                assertion(guest, "public DNS and HTTPS",
                          '(let ((gnutls-verify-error t) (network-security-level \'high) (nsm-noninteractive t) (url-proxy-services nil)) (let ((response (url-retrieve-synchronously %s t t))) (and response (unwind-protect (with-current-buffer response (= url-http-response-status 200)) (kill-buffer response)))))' % json.dumps(internet_url))
            guest.evaluate('(progn (cancel-timer linux-timer) (message "LINUX-%s" "CLEANED"))', "LINUX-CLEANED")
            guest.shutdown()
            guest.close()
            checks.append("first boot clean unmount and poweroff")
            result["disk_after_shutdown_sha256"] = digest(disk)
            guest = Guest(disk, OUT / "second-boot.transcript")
            guests.append(guest)
            guest.wait(b"Your workspace is ready.")
            result["boot_seconds"].append(round(time.monotonic() - guest.started, 3))
            assertion(guest, "source and backup retained across a new QEMU boot",
                      '(and (with-temp-buffer (insert-file-contents "/home/emacs/work/probe.el") (equal (buffer-string) %s)) (with-temp-buffer (insert-file-contents "/home/emacs/work/probe.el~") (equal (buffer-string) %s)))' % (json.dumps(source + ";; second revision\n"), json.dumps(source)))
            assertion(guest, "retained Elisp executes after reboot",
                      f'(progn (load-file "/home/emacs/work/probe.el") (equal linux-probe "{nonce}"))')
            guest.shutdown()
            checks.append("second boot clean unmount and poweroff")
            result["passed"] = True
    except Exception as error:
        result["error"] = str(error)
        raise
    finally:
        for guest in guests:
            if guest.selector.get_map() is not None:
                guest.close()
        result["transcripts"] = {guest.path.name: digest(guest.path) for guest in guests}
        result["disk_sha256"] = digest(disk) if disk.exists() else None
        write_json(OUT / "acceptance.json", result)
    print("Linux guest acceptance passed", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--internet-url", default=os.environ.get("LINUX_INTERNET_URL"))
    args = parser.parse_args()
    if args.internet_url and urlsplit(args.internet_url).scheme != "https":
        parser.error("The internet check requires an https:// URL")
    check(args.internet_url)


if __name__ == "__main__":
    main()
