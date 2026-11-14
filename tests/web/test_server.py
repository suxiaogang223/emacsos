"""Validate the browser server boundary and immutable asset cache behavior."""
import hashlib
import http.client
import importlib.util
import json
from pathlib import Path
import shutil
import ssl
import subprocess
import tempfile
import threading
import unittest
from unittest.mock import patch

SCRIPTS = Path(__file__).resolve().parents[2] / "scripts"


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    loaded = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(loaded)
    return loaded


server = module("serve_web", SCRIPTS / "serve-web.py")
fetcher = module("fetch_web", SCRIPTS / "fetch-web-assets.py")


class ServerTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory()
        cls.root = Path(cls.temporary.name)
        files = {"web/index.html": b"<title>emacsOS</title>", "web/machine.js": b"export {};", "web/terminal.css": b"body{}", "build/mvp/emacsos.img": b"disk", "build/mvp/manifest.json": b"{}", "build/web-vendor/out.js": b"module", "build/web-vendor/machine.wasm": b"wasm", "build/web-vendor/rom.data": b"rom", "web/licenses/notice.txt": b"notice", ".git/config": b"must stay private", "AGENTS.md": b"must stay private"}
        for name, data in files.items():
            target = cls.root / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(data)
        (cls.root / "web/assets.json").write_text(json.dumps({"assets": {"out.js": {}, "machine.wasm": {}, "rom.data": {}}}))
        cls.httpd = server.ThreadingHTTPServer(("127.0.0.1", 0), server.handler_for(cls.root))
        cls.thread = threading.Thread(target=cls.httpd.serve_forever, daemon=True)
        cls.thread.start()

    @classmethod
    def tearDownClass(cls):
        cls.httpd.shutdown()
        cls.httpd.server_close()
        cls.thread.join()
        cls.temporary.cleanup()

    def request(self, path, method="GET"):
        connection = http.client.HTTPConnection("127.0.0.1", self.httpd.server_port)
        connection.request(method, path)
        response = connection.getresponse()
        result = response.status, dict(response.getheaders()), response.read()
        connection.close()
        return result

    def test_allowlisted_routes_and_mime(self):
        for path, mime in [("/", "text/html; charset=utf-8"), ("/machine.js", "text/javascript; charset=utf-8"), ("/vendor/machine.wasm", "application/wasm"), ("/vendor/rom.data", "application/octet-stream"), ("/emacsos.img", "application/octet-stream"), ("/build.json", "application/json"), ("/licenses/notice.txt", "text/plain; charset=utf-8")]:
            with self.subTest(path=path):
                status, headers, body = self.request(path)
                self.assertEqual(status, 200)
                self.assertEqual(headers["Content-Type"], mime)
                self.assertEqual(int(headers["Content-Length"]), len(body))
                self.assertEqual(headers["Cross-Origin-Opener-Policy"], "same-origin")
                self.assertEqual(headers["Cross-Origin-Embedder-Policy"], "require-corp")
                self.assertEqual(headers["Cross-Origin-Resource-Policy"], "same-origin")
                self.assertEqual(headers["X-Content-Type-Options"], "nosniff")

    def test_head_preserves_length_without_body(self):
        status, headers, body = self.request("/emacsos.img", "HEAD")
        self.assertEqual((status, headers["Content-Length"], body), (200, "4", b""))

    def test_source_git_and_traversal_not_exposed(self):
        for path in ["/.git/config", "/AGENTS.md", "/../AGENTS.md", "/%2e%2e/.git/config", "/vendor/../../.git/config", "/vendor/%2e%2e%2f.git/config", "/vendor/", "/web/machine.js", "/build/runtime/emacs", "/vendor/out.js/.."]:
            with self.subTest(path=path):
                status, headers, body = self.request(path)
                self.assertEqual(status, 404)
                self.assertNotIn(b"must stay private", body)
                self.assertEqual(headers["Cross-Origin-Embedder-Policy"], "require-corp")

    def test_queries_do_not_expand_routes(self):
        self.assertEqual(self.request("/vendor/out.js?cache=1")[0], 200)
        self.assertEqual(self.request("/?path=/.git/config")[2], b"<title>emacsOS</title>")

    @unittest.skipUnless(shutil.which("openssl"), "openssl required to create a temporary TLS certificate")
    def test_https_serves_same_isolation_headers(self):
        with tempfile.TemporaryDirectory() as directory:
            cert, key = Path(directory) / "cert.pem", Path(directory) / "key.pem"
            subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-keyout", str(key), "-out", str(cert), "-days", "1", "-subj", "/CN=localhost"], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            context.load_cert_chain(cert, key)
            https = server.ThreadingHTTPServer(("127.0.0.1", 0), server.handler_for(self.root))
            https.socket = context.wrap_socket(https.socket, server_side=True)
            thread = threading.Thread(target=https.serve_forever, daemon=True)
            thread.start()
            try:
                connection = http.client.HTTPSConnection("127.0.0.1", https.server_port, context=ssl._create_unverified_context())
                connection.request("GET", "/")
                response = connection.getresponse()
                self.assertEqual(response.status, 200)
                self.assertEqual(response.getheader("Cross-Origin-Embedder-Policy"), "require-corp")
                response.read()
                connection.close()
            finally:
                https.shutdown(); https.server_close(); thread.join()


class AssetTests(unittest.TestCase):
    def test_valid_cached_asset_does_not_download(self):
        with tempfile.TemporaryDirectory() as directory:
            target = Path(directory) / "module.js"
            target.write_bytes(b"known asset")
            manifest = {"assets": {target.name: {"url": "https://example.test/module.js", "sha256": hashlib.sha256(target.read_bytes()).hexdigest()}}}
            with patch.object(fetcher.subprocess, "run") as run:
                fetcher.fetch_assets(manifest, Path(directory))
                run.assert_not_called()

    def test_bad_download_does_not_replace_cache(self):
        with tempfile.TemporaryDirectory() as directory:
            target = Path(directory) / "module.js"
            target.write_bytes(b"old")
            manifest = {"assets": {target.name: {"url": "https://example.test/module.js", "sha256": hashlib.sha256(b"wanted").hexdigest()}}}
            def corrupt(args, **kwargs):
                Path(args[-1]).write_bytes(b"corrupt")
            with patch.object(fetcher.subprocess, "run", side_effect=corrupt):
                with self.assertRaisesRegex(ValueError, "checksum"):
                    fetcher.fetch_assets(manifest, Path(directory))
            self.assertEqual(target.read_bytes(), b"old")
            self.assertEqual(list(Path(directory).glob("*.part")), [])

    def test_asset_names_cannot_escape_destination(self):
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaises(ValueError):
                fetcher.fetch_assets({"assets": {"../outside": {}}}, Path(directory))


if __name__ == "__main__":
    unittest.main()
