#!/usr/bin/env python3
"""Exercise standard Elisp TCP and retained HTTP source in the actual guest."""
import hashlib
import json
import shutil
import socket
import socketserver
import threading
import time

from check import Guest, Control, OUT, ROOT, identity, qemu, validated_manifest

SOURCE = b"(defun network-retained-answer () (+ 19 23))\n(provide 'network-retained)\n"
SOURCE_HASH = hashlib.sha256(SOURCE).hexdigest()


class Fixture(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


class Handler(socketserver.BaseRequestHandler):
    def handle(self):
        self.request.settimeout(25)
        if self.server.mode == "echo":
            total = 0
            while data := self.request.recv(777):
                total += len(data)
                self.request.sendall(data)
            self.request.sendall(b"half-close-ok")
            self.server.received.append(total)
        else:
            request = b""
            while b"\r\n\r\n" not in request:
                chunk = self.request.recv(1024)
                if not chunk or len(request) > 8192:
                    raise RuntimeError("Invalid controlled HTTP request")
                request += chunk
            self.server.received.append(request.decode("ascii"))
            response = b"HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\nContent-Length: " + str(len(SOURCE)).encode() + b"\r\n\r\n" + SOURCE
            for at in range(0, len(response), 17):
                self.request.sendall(response[at:at + 17])
                time.sleep(0.01)


def main():
    manifest = validated_manifest()
    output = OUT / "network"
    output.mkdir(exist_ok=True)
    servers = []
    for mode in ("echo", "http"):
        server = Fixture(("127.0.0.1", 0), Handler)
        server.mode, server.received = mode, []
        threading.Thread(target=server.serve_forever, daemon=True).start()
        servers.append(server)
    echo, http = servers
    refused = socket.socket()
    refused.bind(("127.0.0.1", 0))  # Bound but not listening, never an external target.
    disk, snapshot = output / "data.img", output / "checkpoint.img"
    disk.unlink(missing_ok=True)
    qemu.prepare_data(disk, OUT / "data-empty.img")
    channel = Control(output)
    guest = Guest(data=disk, control=channel.path, output=output, label="network")
    checks = []
    record = {"parent": manifest["parent"], "fork": manifest["fork"], "lwip": manifest["lwip"],
              "image_sha256": manifest["components"]["build/image/emacsos.img"],
              "source_sha256": SOURCE_HASH, "passed": False, "checks": checks,
              "scope": "Actual native QEMU guest TCP via numeric local fixtures; no DNS/TLS claim"}

    def check(name, form, marker):
        guest.evaluate(form, marker)
        checks.append(name)
        print(f"PASS {name}", flush=True)

    try:
        guest.wait(b"*scratch*")
        guest.wait(b"Network: NE2000 IPv4")
        check("async TCP, 64 KiB echo, write half-close and timer progress",
              '(progn (setq net-data (make-string 65536 ?x) net-reply "" net-ticks 0) '
              '(setq net-timer (run-at-time 0 0.01 (lambda () (setq net-ticks (1+ net-ticks))))) '
              '(setq net-p (make-network-process :name "guest-echo" :host "10.0.2.2" :service %d :family \'ipv4 :nowait t :coding \'binary :noquery t '
              ':sentinel (lambda (p _event) (when (eq (process-status p) \'open) (process-send-string p net-data) (process-send-eof p))) '
              ':filter (lambda (_p bytes) (setq net-reply (concat net-reply bytes)) '
              '(when (= (length net-reply) (+ (length net-data) 13)) (cancel-timer net-timer) '
              '(unless (and (equal net-reply (concat net-data "half-close-ok")) (> net-ticks 0)) (error "Echo differs")) '
              '(message (concat "NET-ECHO-" "PASS")))))) (message "Echo armed"))' % echo.server_address[1], "NET-ECHO-PASS")
        check("connection refusal exposes original errno",
              '(progn (setq net-refused (make-network-process :name "guest-refused" :host "10.0.2.2" :service %d :family \'ipv4 :nowait t :noquery t '
              ':sentinel (lambda (p _e) (when (eq (process-status p) \'failed) (unless (= (process-exit-status p) 111) (error "Wrong refusal")) (message (concat "NET-REFUSED-" "PASS")))))) (message "Refusal armed"))' % refused.getsockname()[1], "NET-REFUSED-PASS")
        check("timeout permits timers and DATA writes",
              '(progn (setq net-wait-ticks 0 net-wait-timer (run-at-time 0 0.05 (lambda () (setq net-wait-ticks (1+ net-wait-ticks)))) '
              'net-timeout (make-network-process :name "guest-timeout" :host "10.0.2.99" :service 9 :family \'ipv4 :nowait t :noquery t '
              ':sentinel (lambda (p _e) (when (eq (process-status p) \'failed) (cancel-timer net-wait-timer) '
              '(unless (and (= (process-exit-status p) 110) (> net-wait-ticks 2) (file-exists-p "/home/user/projects/during-connect.txt")) (error "Timeout or scheduling differs")) '
              '(message (concat "NET-TIMEOUT-" "PASS")))))) '
              '(with-temp-file "/home/user/projects/during-connect.txt" (insert "Editor and DATA remain usable")) (message "Timeout armed"))', "NET-TIMEOUT-PASS")
        check("plain open-network-stream HTTP download, hash, save and explicit load",
              '(progn (require \'network-stream) (setq net-http-buffer (generate-new-buffer " *controlled-http*")) '
              '(setq net-http (open-network-stream "guest-http" net-http-buffer "10.0.2.2" %d :type \'plain :coding \'binary)) '
              '(set-process-query-on-exit-flag net-http nil) (set-process-sentinel net-http #'ignore) (set-process-coding-system net-http \'binary \'binary) '
              '(process-send-string net-http "GET /source.el HTTP/1.0\\r\\nHost: fixture\\r\\n\\r\\n") '
              '(while (memq (process-status net-http) \'(open connect)) (accept-process-output net-http 0.1)) '
              '(with-current-buffer net-http-buffer (goto-char (point-min)) (unless (search-forward "\\r\\n\\r\\n" nil t) (error "Missing HTTP header")) '
              '(setq net-source (buffer-substring-no-properties (point) (point-max)))) '
              '(unless (equal (secure-hash \'sha256 net-source) "%s") (error "Downloaded source differs")) '
              '(with-temp-file "/home/user/projects/downloaded.el" (insert net-source)) '
              '(load-file "/home/user/projects/downloaded.el") (unless (= (network-retained-answer) 42) (error "Downloaded function differs")) '
              '(message (concat "NET-HTTP-" "PASS")))' % (http.server_address[1], SOURCE_HASH), "NET-HTTP-PASS")
        frozen = channel.call(guest, "FREEZE")
        if not frozen["frozen"]:
            raise RuntimeError("No acknowledged DATA freeze")
        shutil.copyfile(disk, snapshot)
        record["checkpoint_sha256"] = identity.digest(snapshot)
        channel.call(guest, "THAW")
        guest.evaluate('(kill-emacs 0)', "Runtime exited status=0x0000000000000000")
        if guest.process.wait(timeout=10) != 33:
            raise RuntimeError("Unclean runtime exit")
        guest.close()
        guest = Guest(data=snapshot, output=output, label="network-reboot")
        guest.wait(b"*scratch*")
        check("fresh boot retains downloaded source without auto-loading it",
              '(progn (when (fboundp \'network-retained-answer) (error "Source auto-loaded")) '
              '(unless (equal (with-temp-buffer (insert-file-contents "/home/user/projects/downloaded.el") (secure-hash \'sha256 (buffer-string))) "%s") (error "Retained source differs")) '
              '(load-file "/home/user/projects/downloaded.el") (unless (= (network-retained-answer) 42) (error "Retained code differs")) '
              '(message (concat "NET-REBOOT-" "PASS")))' % SOURCE_HASH, "NET-REBOOT-PASS")
        guest.evaluate('(kill-emacs 0)', "Runtime exited status=0x0000000000000000")
        if guest.process.wait(timeout=10) != 33:
            raise RuntimeError("Unclean reboot runtime exit")
        if echo.received != [65536] or not http.received or not http.received[0].startswith("GET /source.el HTTP/1.0\r\n"):
            raise RuntimeError("Host fixtures did not observe the expected guest traffic")
        record["fixture_echo_bytes"] = echo.received
        record["fixture_http_requests"] = http.received
        record["passed"] = True
    except Exception as error:
        record["error"] = str(error)
        raise
    finally:
        guest.close()
        channel.close()
        refused.close()
        for server in servers:
            server.shutdown()
            server.server_close()
        (output / "results.json").write_text(json.dumps(record, indent=2) + "\n")


if __name__ == "__main__":
    main()
