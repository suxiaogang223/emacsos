"""Exercise real SLIRP frames, WebSocket boundaries and helper ownership."""
import base64
import hashlib
import http.client
import json
import os
from pathlib import Path
import shutil
import socket
import ssl
import struct
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from unittest.mock import patch

from test_server import module, server, SCRIPTS

network = sys.modules["network"]


def masked(opcode, data=b"", final=True):
    encoded = bytearray(network.frame(opcode, data))
    if not final:
        encoded[0] &= 127
    encoded[1] |= 128
    offset = 2 if len(data) < 126 else (4 if len(data) <= 65535 else 10)
    mask = b"test"
    return bytes(encoded[:offset]) + mask + bytes(value ^ mask[i % 4] for i, value in enumerate(data))


def ethernet():
    packet = bytes.fromhex("ffffffffffff525400123456080600010800060400015254001234560a00020f0000000000000a000202")
    packet += bytes(60 - len(packet))
    return struct.pack("!I", len(packet)) + packet


def exact(connection, size):
    data = b""
    while len(data) < size:
        part = connection.recv(size - len(data))
        if not part:
            raise EOFError
        data += part
    return data


def receive(connection):
    a, b = exact(connection, 2)
    size = b & 127
    if size == 126:
        size = struct.unpack("!H", exact(connection, 2))[0]
    elif size == 127:
        size = struct.unpack("!Q", exact(connection, 8))[0]
    return a & 15, exact(connection, size)


class ParserTests(unittest.TestCase):
    def test_ethernet_splits_and_coalesces(self):
        packet = ethernet()
        parser = network.EthernetFrames()
        self.assertEqual(parser.feed(packet[:2]), [])
        self.assertEqual(parser.feed(packet[2:12]), [])
        self.assertEqual(parser.feed(packet[12:] + packet), [packet, packet])
        self.assertEqual(parser.pending, b"")

    def test_ethernet_rejects_lengths_before_payload(self):
        for size in [0, 13, network.MAX_ETHERNET + 1, 0xffffffff]:
            with self.subTest(size=size), self.assertRaises(network.ProtocolError):
                network.EthernetFrames().feed(struct.pack("!I", size))

    def test_masked_fragmentation_and_interleaved_ping(self):
        parser = network.WebSocketFrames()
        data = masked(2, b"first", False) + masked(9, b"ping") + masked(0, b"last")
        events = []
        for byte in data:
            events.extend(parser.feed(bytes([byte])))
        self.assertEqual(events, [(9, b"ping"), (2, b"firstlast")])

    def test_extended_lengths(self):
        for size in [125, 126, 65535, 65536]:
            payload = b"x" * size
            self.assertEqual(network.WebSocketFrames().feed(masked(2, payload)), [(2, payload)])

    def test_invalid_frames_and_fragment_lifetime(self):
        invalid = [network.frame(2, b"unmasked"), masked(0), masked(1, b"text"),
                   masked(3), masked(9, b"x" * 126), masked(9, b"x", False),
                   masked(2, b"x", False) + masked(2),
                   bytes([0xc2, 0x80]) + b"mask",
                   bytes([0x82, 0xfe, 0, 1]) + b"mask" + b"x",
                   masked(8, b"x"), masked(8, struct.pack("!H", 1005)),
                   masked(8, struct.pack("!H", 1000) + b"\xff")]
        for data in invalid:
            with self.subTest(data=data[:15]), self.assertRaises(network.ProtocolError):
                network.WebSocketFrames().feed(data)

    def test_oversize_frame_and_fragmented_message(self):
        parser = network.WebSocketFrames()
        with self.assertRaises(network.ProtocolError) as error:
            parser.feed(bytes([0x82, 0xff]) + struct.pack("!Q", network.MAX_MESSAGE + 1))
        self.assertEqual(error.exception.code, 1009)
        parser = network.WebSocketFrames()
        parser.feed(masked(2, b"x" * 65536, False))
        with self.assertRaises(network.ProtocolError):
            parser.feed(masked(0, b"x"))


class BridgeTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        (self.root / "web").mkdir()
        (self.root / "web/assets.json").write_text('{"assets":{}}')
        self.manager = server.NetworkManager(limit=1)
        self.httpd = server.ThreadingHTTPServer(("127.0.0.1", 0), server.handler_for(self.root))
        self.httpd.network = self.manager
        self.httpd.network_host = "127.0.0.1"
        self.thread = threading.Thread(target=self.httpd.serve_forever, daemon=True)
        self.thread.start()
        self.connections = []

    def tearDown(self):
        for connection in self.connections:
            connection.close()
        self.manager.close()
        self.httpd.shutdown()
        self.httpd.server_close()
        self.thread.join()
        self.assertFalse(self.manager.sessions)
        self.temporary.cleanup()

    def request(self, host=None, origin=None, extra=None):
        host = host or f"127.0.0.1:{self.httpd.server_port}"
        origin = origin or "http://" + host
        connection = socket.create_connection(("127.0.0.1", self.httpd.server_port), timeout=3)
        if getattr(self.httpd, "network_tls", False):
            connection = ssl._create_unverified_context().wrap_socket(connection, server_hostname="localhost")
        self.connections.append(connection)
        headers = {"Host": host, "Origin": origin, "Upgrade": "websocket", "Connection": "keep-alive, Upgrade",
                   "Sec-WebSocket-Key": base64.b64encode(b"0123456789abcdef").decode(),
                   "Sec-WebSocket-Version": "13", "Sec-WebSocket-Protocol": "binary"}
        headers.update(extra or {})
        request = "GET /network HTTP/1.1\r\n" + "".join(f"{key}: {value}\r\n" for key, value in headers.items() if value is not None) + "\r\n"
        connection.sendall(request.encode())
        response = b""
        while not response.endswith(b"\r\n\r\n"):
            response += exact(connection, 1)
        return connection, response

    def await_cleanup(self, process):
        deadline = time.monotonic() + 3
        while self.manager.sessions and time.monotonic() < deadline:
            time.sleep(.01)
        self.assertEqual(len(self.manager.sessions), 0)
        self.assertIsNotNone(process.poll())

    def test_metadata_does_not_start_a_helper(self):
        connection = http.client.HTTPConnection("127.0.0.1", self.httpd.server_port)
        connection.request("GET", "/network")
        response = connection.getresponse()
        self.assertEqual(response.status, 200)
        self.assertEqual(json.loads(response.read())["path"], "/network")
        connection.close()
        self.assertFalse(self.manager.sessions)

    def test_rejects_cross_origin_dns_rebinding_and_bad_upgrades(self):
        cases = [{"origin": "http://evil.example"}, {"host": "evil.example", "origin": "http://evil.example"},
                 {"extra": {"Origin": None}}, {"extra": {"Sec-WebSocket-Key": "wrong"}},
                 {"extra": {"Sec-WebSocket-Version": "12"}}, {"extra": {"Sec-WebSocket-Protocol": "text"}},
                 {"extra": {"Connection": "keep-alive"}}, {"origin": "https://127.0.0.1"}]
        for case in cases:
            with self.subTest(case=case):
                connection, response = self.request(**case)
                self.assertIn(b"403", response.split(b"\r\n")[0])
                connection.close()
        self.assertFalse(self.manager.sessions)

    @unittest.skipUnless(shutil.which("qemu-system-x86_64"), "native QEMU required")
    def test_real_slirp_split_ws_messages_fragments_ping_and_close(self):
        # A forwarded localhost port differs from the server's own listening port.
        connection, response = self.request(host="localhost:18081", origin="http://localhost:18081")
        self.assertIn(b"101 Switching Protocols", response)
        self.assertIn(b"Sec-WebSocket-Protocol: binary", response)
        process = next(iter(self.manager.sessions)).process
        packet = ethernet()
        connection.sendall(masked(2, packet[:2]))
        connection.sendall(masked(2, packet[2:20], False) + masked(9, b"alive") + masked(0, packet[20:]))
        observed = {}
        for _ in range(2):
            opcode, payload = receive(connection)
            observed[opcode] = payload
        self.assertEqual(observed[10], b"alive")
        arp = observed[2]
        self.assertEqual(struct.unpack("!I", arp[:4])[0], 64)
        self.assertEqual(arp[4 + 20:4 + 22], b"\0\2")
        self.assertEqual(arp[4 + 28:4 + 32], bytes([10, 0, 2, 2]))
        connection.sendall(masked(8, struct.pack("!H", 1000)))
        self.assertEqual(receive(connection), (8, struct.pack("!H", 1000)))
        self.await_cleanup(process)

    @unittest.skipUnless(shutil.which("qemu-system-x86_64"), "native QEMU required")
    def test_malformed_ethernet_closes_and_reaps(self):
        connection, _ = self.request()
        process = next(iter(self.manager.sessions)).process
        connection.sendall(masked(2, struct.pack("!I", 0xffffffff)))
        self.assertEqual(receive(connection), (8, struct.pack("!H", 1002)))
        self.await_cleanup(process)

    @unittest.skipUnless(shutil.which("qemu-system-x86_64"), "native QEMU required")
    def test_process_limit_disconnect_and_server_shutdown(self):
        connection, _ = self.request()
        process = next(iter(self.manager.sessions)).process
        rejected, response = self.request()
        self.assertIn(b"503", response)
        rejected.close()
        connection.close()
        self.await_cleanup(process)
        connection, response = self.request()
        self.assertIn(b"101 Switching Protocols", response)
        process = next(iter(self.manager.sessions)).process
        self.manager.close()
        self.assertIsNotNone(process.poll())

    def test_missing_backend_and_spawn_failure_leave_no_session(self):
        with patch.object(self.manager, "executable", None):
            _, response = self.request()
            self.assertIn(b"503", response)
        with patch.object(network.subprocess, "Popen", side_effect=OSError("cannot start")):
            _, response = self.request()
            self.assertIn(b"503", response)
        self.assertFalse(self.manager.sessions)

    @unittest.skipUnless(shutil.which("qemu-system-x86_64"), "native QEMU required")
    def test_queue_limit_fails_closed_and_reaps(self):
        connection, _ = self.request()
        process = next(iter(self.manager.sessions)).process
        with patch.object(network, "MAX_PENDING", 32):
            connection.sendall(masked(2, ethernet()))
            self.assertEqual(receive(connection), (8, struct.pack("!H", 1013)))
        self.await_cleanup(process)

    @unittest.skipUnless(shutil.which("qemu-system-x86_64") and shutil.which("openssl"), "QEMU and openssl required")
    def test_tls_websocket_transfers_real_frames(self):
        cert, key = self.root / "cert.pem", self.root / "key.pem"
        subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-keyout", str(key), "-out", str(cert), "-days", "1", "-subj", "/CN=localhost"], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(cert, key)
        self.httpd.shutdown()
        self.thread.join()
        self.httpd.socket = context.wrap_socket(self.httpd.socket, server_side=True)
        self.httpd.network_tls = True
        self.thread = threading.Thread(target=self.httpd.serve_forever, daemon=True)
        self.thread.start()
        connection, response = self.request(origin=f"https://127.0.0.1:{self.httpd.server_port}")
        self.assertIn(b"101 Switching Protocols", response)
        process = next(iter(self.manager.sessions)).process
        for byte in masked(2, ethernet()):
            connection.sendall(bytes([byte]))
        opcode, reply = receive(connection)
        self.assertEqual(opcode, 2)
        self.assertEqual(reply[4 + 20:4 + 22], b"\0\2")
        connection.close()
        self.await_cleanup(process)

    @unittest.skipUnless(shutil.which("qemu-system-x86_64") and shutil.which("openssl"), "QEMU and openssl required")
    def test_tls_backpressure_preserves_queued_frames(self):
        cert, key = self.root / "cert.pem", self.root / "key.pem"
        subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-keyout", str(key), "-out", str(cert), "-days", "1", "-subj", "/CN=localhost"], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(cert, key)
        self.httpd.shutdown()
        self.thread.join()
        self.httpd.socket = context.wrap_socket(self.httpd.socket, server_side=True)
        self.httpd.network_tls = True
        self.thread = threading.Thread(target=self.httpd.serve_forever, daemon=True)
        self.thread.start()
        connection, response = self.request(origin=f"https://127.0.0.1:{self.httpd.server_port}")
        self.assertIn(b"101 Switching Protocols", response)
        session = next(iter(self.manager.sessions))
        session.connection.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 1024)
        connection.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1024)
        count = 700
        connection.sendall(masked(2, ethernet() * (count // 2)))
        time.sleep(.2)
        connection.sendall(masked(2, ethernet() * (count // 2)))
        time.sleep(.2)
        for index in range(count):
            opcode, reply = receive(connection)
            self.assertEqual(opcode, 2, f"reply {index}: {reply.hex()}")
            self.assertEqual(reply[4 + 20:4 + 22], b"\0\2")
        connection.close()
        self.await_cleanup(session.process)

    @unittest.skipUnless(shutil.which("qemu-system-x86_64"), "native QEMU required")
    def test_backend_failure_closes_websocket(self):
        connection, _ = self.request()
        process = next(iter(self.manager.sessions)).process
        process.terminate()
        opcode, _ = receive(connection)
        self.assertEqual(opcode, 8)
        self.await_cleanup(process)


if __name__ == "__main__":
    unittest.main()
