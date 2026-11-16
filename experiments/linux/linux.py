#!/usr/bin/env python3
"""Build, boot and accept the separate Linux exploration on native QEMU."""
import argparse
import contextlib
import hashlib
import http.server
import json
import os
from pathlib import Path
import secrets
import selectors
import shutil
import ssl
import subprocess
import tempfile
import threading
import time
from urllib.parse import urlsplit

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
OUT = ROOT / "build/linux"
IMAGE = "emacsos-linux-spike:alpine3.22"
INPUTS = ("Dockerfile", "packages.lock", "init", "inittab", "boot", "start-emacs",
          "desktop.el", "linux.py")


def run(*args, **kwargs):
    return subprocess.run([str(arg) for arg in args], check=True, **kwargs)


def git(*args):
    return subprocess.check_output(["git", "-C", str(ROOT), *args], text=True).strip()


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def inputs():
    return {name: digest(HERE / name) for name in INPUTS}


def write_json(path, data):
    path.write_text(json.dumps(data, indent=2) + "\n")


def create_disk(path, certificate=None):
    """Create only a new dedicated disk; never format an existing user disk."""
    with path.open("xb") as disk:
        disk.truncate(64 * 1024 * 1024)
    with tempfile.TemporaryDirectory(prefix=".seed-", dir=OUT) as temporary:
        seed = Path(temporary)
        (seed / "emacs/work").mkdir(parents=True)
        if certificate:
            shutil.copyfile(certificate, seed / "emacs/work/fixture-ca.pem")
        run("mkfs.ext4", "-q", "-F", "-m", "0", "-L", "EMACSOS_DATA", "-d", seed, path)
    for directory in ("/emacs", "/emacs/work"):
        for field in ("uid", "gid"):
            run("debugfs", "-w", "-R", f"set_inode_field {directory} {field} 1000", path,
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def build():
    OUT.mkdir(parents=True, exist_ok=True)
    run("docker", "build", "--platform", "linux/amd64", "-t", IMAGE, HERE)
    # The container supplies root ownership and devices are created at guest boot.
    # The archive never includes Docker's mounted /dev, /proc, /sys or /out.
    package = r'''
set -eu
set -o pipefail
cp /boot/vmlinuz-virt /out/vmlinuz
find bin etc home init lib media mnt opt root run sbin srv tmp usr var -xdev -print0 > /out/archive.list
printf 'dev\0proc\0sys\0' >> /out/archive.list
sort -z /out/archive.list | cpio --null -o -H newc --owner=0:0 2>/out/cpio.log | gzip -n > /out/initramfs.cpio.gz
apk list --installed > /out/packages.txt
emacs --batch -Q --eval '(princ (format "%s\n%s\n" emacs-version system-configuration-options))' > /out/emacs.txt
chown "$OUTPUT_UID:$OUTPUT_GID" /out/vmlinuz /out/initramfs.cpio.gz /out/packages.txt /out/emacs.txt /out/cpio.log /out/archive.list
'''
    run("docker", "run", "--rm", "--platform", "linux/amd64",
        "-e", f"OUTPUT_UID={os.getuid()}", "-e", f"OUTPUT_GID={os.getgid()}",
        "-v", f"{OUT}:/out:z", IMAGE, "/bin/sh", "-c", package)
    container = subprocess.check_output(
        ["docker", "image", "inspect", IMAGE, "--format", "{{.Id}}"], text=True).strip()
    write_json(OUT / "manifest.json", {
        "parent": git("rev-parse", "HEAD"), "dirty": bool(git("status", "--porcelain")),
        "fork_pin": git("rev-parse", "HEAD:emacs"),
        "runtime": "Alpine emacs-nox 30.1-r1; not the pinned Emacs fork",
        "container": container, "inputs": inputs(),
        "artifacts": {name: digest(OUT / name) for name in
                      ("vmlinuz", "initramfs.cpio.gz", "packages.txt", "emacs.txt")},
        "machine": "native QEMU pc, x86-64 TCG, 1 GiB, COM1, virtio block/net, user network",
    })
    if not (OUT / "data.ext4").exists():
        create_disk(OUT / "data.ext4")
    print(f"Linux exploration ready: {OUT}", flush=True)


def verify():
    manifest = json.loads((OUT / "manifest.json").read_text())
    if manifest["inputs"] != inputs():
        raise RuntimeError("Linux exploration sources changed; run linux-build")
    for name, expected in manifest["artifacts"].items():
        if digest(OUT / name) != expected:
            raise RuntimeError(f"Linux exploration artifact changed: {name}")
    return manifest


def command(disk):
    return ["qemu-system-x86_64", "-machine", "pc", "-accel", "tcg", "-m", "1024",
            "-kernel", str(OUT / "vmlinuz"), "-initrd", str(OUT / "initramfs.cpio.gz"),
            "-append", "console=ttyS0,115200 rdinit=/init panic=-1 quiet",
            "-drive", f"file={disk},format=raw,if=virtio",
            "-netdev", "user,id=net0", "-device", "virtio-net-pci,netdev=net0",
            "-display", "none", "-serial", "stdio", "-monitor", "none", "-no-reboot"]


class Guest:
    def __init__(self, disk, transcript):
        self.path = transcript
        self.transcript = bytearray()
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
        raise RuntimeError("Commit the exploration before recording guest acceptance")
    if manifest["parent"] != git("rev-parse", "HEAD"):
        raise RuntimeError("Rebuild at the current source commit before acceptance")
    checks, guests = [], []
    result = {"passed": False, "parent": manifest["parent"], "checks": checks,
              "manifest_sha256": digest(OUT / "manifest.json"),
              "scope": "Native QEMU Linux exploration only; separate from the own-kernel MVP",
              "internet_url": internet_url}

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
    try:
        with https_fixture() as (certificate, url, nonce):
            create_disk(disk, certificate)
            guest = Guest(disk, OUT / "first-boot.transcript")
            guests.append(guest)
            guest.wait(b"EMACSOS-LINUX-READY")
            assertion(guest, "interactive Linux Emacs as uid 1000",
                      "(and (eq system-type 'gnu/linux) (= (user-uid) 1000) (equal emacs-version \"30.1\"))")
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
                       '(let ((response (url-retrieve-synchronously %s t t 20))) '
                       '(unless response (error "No HTTPS response")) '
                       '(unwind-protect (with-current-buffer response (goto-char (point-min)) '
                       '(and (= url-http-response-status 200) (search-forward %s nil t))) (kill-buffer response))))')
            assertion(guest, "HTTPS with certificate verification and unpredictable response",
                      request % (json.dumps(url), json.dumps(nonce)))
            assertion(guest, "untrusted HTTPS certificate rejected",
                      '(progn (setq gnutls-trustfiles nil) (condition-case failure (progn (open-network-stream "untrusted" nil "10.0.2.2" %d :type \'tls) nil) (error (string-match-p "Certificate validation failed" (error-message-string failure)))))' % urlsplit(url).port)
            guest.evaluate('(progn (setq gnutls-trustfiles linux-system-trustfiles) (message "LINUX-%s" "TRUST-RESTORED"))', "LINUX-TRUST-RESTORED")
            if internet_url:
                assertion(guest, "public DNS and HTTPS",
                          '(let ((gnutls-verify-error t) (network-security-level \'high) (nsm-noninteractive t) (url-proxy-services nil)) (let ((response (url-retrieve-synchronously %s t t 30))) (and response (unwind-protect (with-current-buffer response (= url-http-response-status 200)) (kill-buffer response)))))' % json.dumps(internet_url))
            guest.evaluate('(progn (cancel-timer linux-timer) (message "LINUX-%s" "CLEANED"))', "LINUX-CLEANED")
            guest.shutdown()
            guest.close()
            checks.append("first boot clean unmount and poweroff")
            result["disk_after_shutdown_sha256"] = digest(disk)
            guest = Guest(disk, OUT / "second-boot.transcript")
            guests.append(guest)
            guest.wait(b"EMACSOS-LINUX-READY")
            assertion(guest, "source and backup retained across a new QEMU boot",
                      '(and (with-temp-buffer (insert-file-contents "/home/emacs/work/probe.el") (equal (buffer-string) %s)) (file-exists-p "/home/emacs/work/probe.el~"))' % json.dumps(source + ";; second revision\n"))
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
    print("Linux exploration guest acceptance passed", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("build", "run", "check"))
    parser.add_argument("--internet-url", default=os.environ.get("LINUX_INTERNET_URL"),
                        help="Also require public DNS/HTTPS for this URL")
    args = parser.parse_args()
    if args.internet_url and urlsplit(args.internet_url).scheme != "https":
        parser.error("The optional internet check requires an https:// URL")
    if args.action == "build":
        build()
    elif args.action == "run":
        verify()
        run(*command(OUT / "data.ext4"))
    else:
        check(args.internet_url)


if __name__ == "__main__":
    main()
