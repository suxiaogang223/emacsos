"""Bounded same-origin Ethernet WebSocket relay to a private QEMU SLIRP backend."""
import base64
import hashlib
import ipaddress
import select
import shutil
import socket
import ssl
import struct
import subprocess
import threading
from urllib.parse import urlsplit

MAX_ETHERNET = 2048
MAX_MESSAGE = 65536
MAX_PENDING = 262144
MAX_MACHINES = 4


class ProtocolError(Exception):
    def __init__(self, message, code=1002):
        super().__init__(message)
        self.code = code


class Unavailable(Exception):
    pass


def frame(opcode, data=b""):
    size = len(data)
    header = bytes([0x80 | opcode])
    if size < 126:
        return header + bytes([size]) + data
    if size <= 65535:
        return header + b"\x7e" + struct.pack("!H", size) + data
    return header + b"\x7f" + struct.pack("!Q", size) + data


class EthernetFrames:
    """QEMU socket framing is independent of WebSocket message boundaries."""
    def __init__(self):
        self.pending = bytearray()

    def feed(self, data):
        self.pending.extend(data)
        packets = []
        while len(self.pending) >= 4:
            size = struct.unpack_from("!I", self.pending)[0]
            if not 14 <= size <= MAX_ETHERNET:
                raise ProtocolError("Invalid Ethernet frame length")
            if len(self.pending) < 4 + size:
                break
            packets.append(bytes(self.pending[:4 + size]))
            del self.pending[:4 + size]
        return packets


class WebSocketFrames:
    """Parse masked client frames with bounded binary-message fragmentation."""
    def __init__(self):
        self.pending = bytearray()
        self.message = None

    def feed(self, data):
        self.pending.extend(data)
        events = []
        while len(self.pending) >= 2:
            first, second = self.pending[:2]
            final, opcode = bool(first & 128), first & 15
            if first & 112 or not second & 128:
                raise ProtocolError("Reserved bits or unmasked client frame")
            size, offset = second & 127, 2
            if size == 126:
                if len(self.pending) < 4:
                    break
                size, offset = struct.unpack_from("!H", self.pending, 2)[0], 4
                if size < 126:
                    raise ProtocolError("Non-minimal frame length")
            elif size == 127:
                if len(self.pending) < 10:
                    break
                size, offset = struct.unpack_from("!Q", self.pending, 2)[0], 10
                if size < 65536 or size >> 63:
                    raise ProtocolError("Invalid frame length")
            control = opcode >= 8
            if opcode not in (0, 1, 2, 8, 9, 10) or (control and (not final or size > 125)):
                raise ProtocolError("Invalid WebSocket opcode or control frame")
            if size > MAX_MESSAGE:
                raise ProtocolError("WebSocket message too large", 1009)
            if len(self.pending) < offset + 4 + size:
                break
            mask = self.pending[offset:offset + 4]
            offset += 4
            payload = bytes(value ^ mask[index % 4] for index, value in enumerate(self.pending[offset:offset + size]))
            del self.pending[:offset + size]
            if opcode == 8:
                if len(payload) == 1:
                    raise ProtocolError("Invalid close payload")
                if len(payload) >= 2:
                    code = struct.unpack_from("!H", payload)[0]
                    if not (code in (1000, 1001, 1002, 1003, 1007, 1008, 1009, 1010, 1011, 1012, 1013, 1014) or 3000 <= code <= 4999):
                        raise ProtocolError("Invalid close code")
                    try:
                        payload[2:].decode("utf-8")
                    except UnicodeError as error:
                        raise ProtocolError("Invalid close reason", 1007) from error
                events.append((opcode, payload))
                break
            if control:
                events.append((opcode, payload))
                continue
            if opcode == 1:
                raise ProtocolError("Binary messages required", 1003)
            if opcode == 2:
                if self.message is not None:
                    raise ProtocolError("Interleaved fragmented messages")
                self.message = bytearray()
            elif self.message is None:
                raise ProtocolError("Continuation without message")
            if len(self.message) + size > MAX_MESSAGE:
                raise ProtocolError("WebSocket message too large", 1009)
            self.message.extend(payload)
            if final:
                events.append((2, bytes(self.message)))
                self.message = None
        return events


def valid_request(handler):
    """Require a browser upgrade from this explicit local/listening authority."""
    headers = handler.headers
    for name in ("Host", "Origin", "Upgrade", "Sec-WebSocket-Key", "Sec-WebSocket-Version"):
        if len(headers.get_all(name, [])) != 1:
            return False
    try:
        host = urlsplit("//" + headers["Host"])
        origin = urlsplit(headers["Origin"])
        if not host.hostname or host.username or host.password or host.path or host.query or host.fragment:
            return False
        if host.port is not None and not 1 <= host.port <= 65535:
            return False
        expected_scheme = "https" if getattr(handler.server, "network_tls", False) else "http"
        if origin.scheme != expected_scheme or origin.netloc.lower() != headers["Host"].lower() or origin.path or origin.query or origin.fragment:
            return False
        allowed = {"localhost", handler.connection.getsockname()[0].lower()}
        configured = getattr(handler.server, "network_host", "")
        if configured not in ("", "0.0.0.0", "::"):
            allowed.add(configured.lower())
        try:
            loopback = ipaddress.ip_address(host.hostname).is_loopback
        except ValueError:
            loopback = False
        if host.hostname.lower() not in allowed and not loopback:
            return False
        key = headers["Sec-WebSocket-Key"]
        if len(base64.b64decode(key, validate=True)) != 16:
            return False
    except (ValueError, UnicodeError):
        return False
    connection = {token.strip().lower() for value in headers.get_all("Connection", []) for token in value.split(",")}
    protocols = {token.strip() for value in headers.get_all("Sec-WebSocket-Protocol", []) for token in value.split(",")}
    return ("upgrade" in connection and headers["Upgrade"].lower() == "websocket"
            and headers["Sec-WebSocket-Version"] == "13" and "binary" in protocols)


class NetworkManager:
    def __init__(self, executable=None, limit=MAX_MACHINES):
        self.executable = executable or shutil.which("qemu-system-x86_64")
        self.limit = limit
        self.sessions = set()
        self.lock = threading.Lock()
        self.closed = False

    @property
    def available(self):
        return bool(self.executable) and not self.closed

    def open(self, connection):
        with self.lock:
            if not self.available or len(self.sessions) >= self.limit:
                raise Unavailable("Network unavailable or machine limit reached")
            session = NetworkSession(self, connection)
            self.sessions.add(session)
            return session

    def release(self, session):
        with self.lock:
            self.sessions.discard(session)

    def close(self):
        with self.lock:
            self.closed = True
            sessions = list(self.sessions)
        for session in sessions:
            session.stop()


class NetworkSession:
    def __init__(self, manager, connection):
        self.manager, self.connection = manager, connection
        self.stop_lock = threading.Lock()
        self.stopped = False
        self.channel, peer = socket.socketpair()
        args = [manager.executable, "-machine", "none", "-nodefaults", "-display", "none",
                "-monitor", "none", "-serial", "none", "-netdev", "user,id=nat",
                "-netdev", f"socket,id=wire,fd={peer.fileno()}",
                "-netdev", "hubport,id=h0,hubid=0,netdev=nat",
                "-netdev", "hubport,id=h1,hubid=0,netdev=wire"]
        try:
            self.process = subprocess.Popen(args, pass_fds=(peer.fileno(),), stdin=subprocess.DEVNULL,
                                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        except OSError:
            self.channel.close()
            raise
        finally:
            peer.close()

    def stop(self):
        with self.stop_lock:
            if self.stopped:
                return
            self.stopped = True
            for channel in (self.channel, self.connection):
                try:
                    channel.shutdown(socket.SHUT_RDWR)
                except OSError:
                    pass
            self.channel.close()
            if self.process.poll() is None:
                self.process.terminate()
                try:
                    self.process.wait(timeout=1)
                except subprocess.TimeoutExpired:
                    self.process.kill()
                    self.process.wait()
        self.manager.release(self)

    def relay(self):
        client, backend = self.connection, self.channel
        client.setblocking(False)
        backend.setblocking(False)
        websocket, outgoing, incoming = WebSocketFrames(), EthernetFrames(), EthernetFrames()
        to_backend, to_client = bytearray(), bytearray()
        client_chunk = b""
        close_payload = struct.pack("!H", 1001)
        try:
            while not self.stopped:
                if self.process.poll() is not None:
                    raise ProtocolError("Network backend stopped", 1011)
                readable, writable, _ = select.select([client, backend],
                    ([backend] if to_backend else []) + ([client] if to_client or client_chunk else []), [], 0.25)
                if isinstance(client, ssl.SSLSocket) and client.pending() and client not in readable:
                    readable.append(client)
                for channel in readable:
                    try:
                        data = channel.recv(16384)
                    except (BlockingIOError, ssl.SSLWantReadError, ssl.SSLWantWriteError):
                        continue
                    if not data:
                        return
                    if channel is backend:
                        for packet in incoming.feed(data):
                            to_client.extend(frame(2, packet))
                    else:
                        for opcode, payload in websocket.feed(data):
                            if opcode == 8:
                                close_payload = payload
                                return
                            if opcode == 9:
                                to_client.extend(frame(10, payload))
                            if opcode == 2:
                                for packet in outgoing.feed(payload):
                                    to_backend.extend(packet)
                    if len(to_client) + len(client_chunk) > MAX_PENDING or len(to_backend) > MAX_PENDING:
                        raise ProtocolError("Network queue limit reached", 1013)
                for channel in writable:
                    if channel is client and not client_chunk:
                        client_chunk = bytes(to_client[:16384])
                        del to_client[:len(client_chunk)]
                    # SSLWantWrite requires the same bytes on the next retry;
                    # newly received frames must not alter this in-flight write.
                    pending = client_chunk if channel is client else to_backend
                    try:
                        sent = channel.send(pending)
                    except (BlockingIOError, ssl.SSLWantReadError, ssl.SSLWantWriteError):
                        continue
                    if sent <= 0:
                        return
                    if channel is client:
                        client_chunk = client_chunk[sent:]
                    else:
                        del to_backend[:sent]
        except ProtocolError as error:
            close_payload = struct.pack("!H", error.code)
        except (OSError, ValueError):
            close_payload = struct.pack("!H", 1011)
        finally:
            try:
                client.settimeout(0.2)
                client.sendall(frame(8, close_payload))
            except OSError:
                pass
            self.stop()


def upgrade(handler, manager):
    if not valid_request(handler):
        handler.send_error(403, "Same-origin binary WebSocket required")
        return
    try:
        session = manager.open(handler.connection)
    except (Unavailable, OSError):
        handler.send_error(503, "Network unavailable")
        return
    try:
        key = handler.headers["Sec-WebSocket-Key"]
        accept = base64.b64encode(hashlib.sha1((key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode("ascii")).digest()).decode("ascii")
        handler.send_response(101, "Switching Protocols")
        handler.send_header("Upgrade", "websocket")
        handler.send_header("Connection", "Upgrade")
        handler.send_header("Sec-WebSocket-Accept", accept)
        handler.send_header("Sec-WebSocket-Protocol", "binary")
        handler.end_headers()
        handler.wfile.flush()
        handler.close_connection = True
        session.relay()
    finally:
        session.stop()
