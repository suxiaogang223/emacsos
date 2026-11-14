#!/usr/bin/env python3
"""Serve only the browser UI, allowlisted vendor filenames and the built MVP image."""
import argparse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import shutil
import ssl
from urllib.parse import unquote, urlsplit

ROOT = Path(__file__).resolve().parent.parent


def routes(root):
    assets = json.loads((root / "web/assets.json").read_text())["assets"]
    result = {
        "/": (root / "web/index.html", "text/html; charset=utf-8"),
        "/index.html": (root / "web/index.html", "text/html; charset=utf-8"),
        "/machine.js": (root / "web/machine.js", "text/javascript; charset=utf-8"),
        "/terminal.css": (root / "web/terminal.css", "text/css; charset=utf-8"),
        "/emacsos.img": (root / "build/mvp/emacsos.img", "application/octet-stream"),
        "/build.json": (root / "build/mvp/manifest.json", "application/json"),
        "/assets.json": (root / "web/assets.json", "application/json"),
        "/licenses.html": (root / "web/licenses.html", "text/html; charset=utf-8"),
    }
    types = {".js": "text/javascript; charset=utf-8", ".css": "text/css; charset=utf-8", ".wasm": "application/wasm"}
    for name in assets:
        if Path(name).name != name or name in (".", ".."):
            raise ValueError("Vendor manifest names must be plain filenames")
        result["/vendor/" + name] = (root / "build/web-vendor" / name, types.get(Path(name).suffix, "application/octet-stream"))
    for path in (root / "web/licenses").glob("*.txt"):
        result["/licenses/" + path.name] = (path, "text/plain; charset=utf-8")
    return result


def handler_for(root):
    allowed = routes(root)

    class Handler(BaseHTTPRequestHandler):
        server_version = "emacsOSWeb/1"
        sys_version = ""

        def end_headers(self):
            self.send_header("Cross-Origin-Opener-Policy", "same-origin")
            self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
            self.send_header("Cross-Origin-Resource-Policy", "same-origin")
            self.send_header("X-Content-Type-Options", "nosniff")
            self.send_header("Cache-Control", "no-cache")
            super().end_headers()

        def do_HEAD(self):
            self.serve(False)

        def do_GET(self):
            self.serve(True)

        def serve(self, body):
            try:
                name = unquote(urlsplit(self.path).path, errors="strict")
            except (UnicodeError, ValueError):
                self.send_error(400)
                return
            target = allowed.get(name)
            if target is None:
                self.send_error(404)
                return
            path, mime = target
            try:
                stream = path.open("rb")
            except (OSError, ValueError):
                self.send_error(404)
                return
            with stream:
                self.send_response(200)
                self.send_header("Content-Type", mime)
                self.send_header("Content-Length", str(os.fstat(stream.fileno()).st_size))
                self.end_headers()
                if body:
                    try:
                        shutil.copyfileobj(stream, self.wfile)
                    except (BrokenPipeError, ConnectionResetError):
                        pass

    return Handler


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=18082)
    parser.add_argument("--cert", type=Path)
    parser.add_argument("--key", type=Path)
    args = parser.parse_args()
    if bool(args.cert) != bool(args.key):
        parser.error("--cert and --key must be provided together")
    if not 1 <= args.port <= 65535:
        parser.error("--port must be between 1 and 65535")
    context = None
    if args.cert:
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(args.cert, args.key)
    server = ThreadingHTTPServer((args.host, args.port), handler_for(ROOT))
    if context:
        server.socket = context.wrap_socket(server.socket, server_side=True)
    print(f"emacsOS: {'https' if context else 'http'}://{args.host}:{server.server_port}/", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
