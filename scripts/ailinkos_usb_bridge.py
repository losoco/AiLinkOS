#!/usr/bin/env python3
"""AirLinkOS wired screen mirror bridge.

The device streams the same ALM frame packets it uses over Wi-Fi, but on the USB
CDC link (the port that also carries the serial console) and with Wi-Fi forced
off. This script opens that port, fetches the viewer page from the firmware,
serves it on http://127.0.0.1:8080 and re-publishes the frames on
ws://127.0.0.1:8081, so the normal viewer page works unchanged over the cable.

Usage:
    python3 scripts/ailinkos_usb_bridge.py [--port /dev/cu.usbmodemXXXX]
                                           [--http-port 8080] [--ws-port 8081]
                                           [--open]

Only the Python standard library is used.
"""

import argparse
import base64
import glob
import hashlib
import http.server
import os
import select
import socket
import struct
import sys
import termios
import threading
import time
import urllib.parse
import webbrowser

WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
FRAME_MAGIC = b"MSF1"
ALM_MAGIC = b"ALM\x02"
BAUD = 115200


def log(message: str) -> None:
    print(f"[bridge] {message}", file=sys.stderr, flush=True)


def find_port() -> str:
    for pattern in ("/dev/cu.usbmodem*", "/dev/ttyACM*", "/dev/tty.usbmodem*"):
        ports = sorted(glob.glob(pattern))
        if ports:
            return ports[0]
    return ""


class SerialLink:
    """Raw USB CDC link. Mirrors esp_log.py: no DTR/RTS toggling on open."""

    def __init__(self, path: str):
        self.fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        iflag, oflag, cflag, lflag, _, _, cc = termios.tcgetattr(self.fd)
        iflag &= ~(termios.IGNBRK | termios.BRKINT | termios.PARMRK | termios.ISTRIP | termios.INLCR |
                   termios.IGNCR | termios.ICRNL | termios.IXON | termios.IXOFF | termios.IXANY)
        oflag &= ~termios.OPOST
        lflag &= ~(termios.ECHO | termios.ECHONL | termios.ICANON | termios.ISIG | termios.IEXTEN)
        cflag &= ~(termios.CSIZE | termios.PARENB | termios.CSTOPB | termios.CRTSCTS)
        cflag |= termios.CS8 | termios.CREAD | termios.CLOCAL
        termios.tcsetattr(self.fd, termios.TCSANOW, [iflag, oflag, cflag, lflag, termios.B115200, termios.B115200, cc])
        self.pending = b""
        self._drain()

    def _drain(self) -> None:
        try:
            while select.select([self.fd], [], [], 0)[0]:
                os.read(self.fd, 4096)
        except OSError:
            pass
        self.pending = b""

    def write(self, data: bytes) -> None:
        os.write(self.fd, data)

    def command(self, line: str) -> None:
        self.write(line.encode("ascii") + b"\n")

    def _fill(self, timeout: float) -> bool:
        ready, _, _ = select.select([self.fd], [], [], timeout)
        if not ready:
            return False
        try:
            chunk = os.read(self.fd, 65536)
        except OSError:
            return False
        if chunk:
            self.pending += chunk
        return bool(chunk)

    def read(self, timeout: float = 0.5) -> bytes:
        """Everything received so far; blocks up to `timeout` for the first byte."""
        if not self.pending:
            self._fill(timeout)
        data, self.pending = self.pending, b""
        return data

    def read_line(self, timeout: float = 3.0) -> str:
        deadline = time.time() + timeout
        while True:
            index = self.pending.find(b"\n")
            if index >= 0:
                line, self.pending = self.pending[:index], self.pending[index + 1:]
                return line.decode("utf-8", "replace").strip()
            if time.time() >= deadline:
                return ""
            self._fill(min(0.2, max(0.0, deadline - time.time())))

    def read_exact(self, count: int, timeout: float = 10.0) -> bytes:
        deadline = time.time() + timeout
        while len(self.pending) < count and time.time() < deadline:
            self._fill(0.2)
        data, self.pending = self.pending[:count], self.pending[count:]
        return data

    def close(self) -> None:
        try:
            os.close(self.fd)
        except OSError:
            pass


class WsClient:
    def __init__(self, sock: socket.socket):
        self.sock = sock
        self.lock = threading.Lock()
        self.closed = False

    def send_frame(self, opcode: int, payload: bytes) -> bool:
        if self.closed:
            return False
        header = bytearray([0x80 | opcode])
        length = len(payload)
        if length < 126:
            header.append(length)
        elif length < 65536:
            header.append(126)
            header += struct.pack(">H", length)
        else:
            header.append(127)
            header += struct.pack(">Q", length)
        with self.lock:
            try:
                self.sock.sendall(bytes(header) + payload)
                return True
            except OSError:
                self.closed = True
                return False

    def close(self) -> None:
        self.closed = True
        try:
            self.sock.close()
        except OSError:
            pass


class WsHub:
    """Minimal RFC6455 server: text in, binary frames out."""

    def __init__(self, port: int, on_text, on_connect=None):
        self.on_text = on_text
        self.on_connect = on_connect
        self.clients = []
        self.lock = threading.Lock()
        self.server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.server.bind(("127.0.0.1", port))
        self.server.listen(4)

    def broadcast(self, payload: bytes) -> None:
        with self.lock:
            clients = list(self.clients)
        for client in clients:
            if not client.send_frame(0x2, payload):
                self.drop(client)

    def drop(self, client: WsClient) -> None:
        with self.lock:
            if client in self.clients:
                self.clients.remove(client)
        client.close()

    def serve_forever(self) -> None:
        while True:
            try:
                conn, _ = self.server.accept()
            except OSError:
                return
            threading.Thread(target=self._handshake, args=(conn,), daemon=True).start()

    def _handshake(self, conn: socket.socket) -> None:
        conn.settimeout(5.0)
        request = b""
        while b"\r\n\r\n" not in request:
            chunk = conn.recv(4096)
            if not chunk:
                conn.close()
                return
            request += chunk
        headers = {}
        for line in request.decode("latin-1").split("\r\n")[1:]:
            if ":" in line:
                key, value = line.split(":", 1)
                headers[key.strip().lower()] = value.strip()
        key = headers.get("sec-websocket-key")
        if not key:
            conn.close()
            return
        accept = base64.b64encode(hashlib.sha1((key + WS_GUID).encode()).digest()).decode()
        conn.sendall(("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                      f"Sec-WebSocket-Accept: {accept}\r\n\r\n").encode())
        client = WsClient(conn)
        with self.lock:
            self.clients.append(client)
        # The page counts a viewer as live only after this acknowledgement.
        client.send_frame(0x1, b"OK")
        if self.on_connect is not None:
            # A new tab has no baseline: patches alone would paint half a screen.
            self.on_connect()
        self._read_loop(client)

    def _read_loop(self, client: WsClient) -> None:
        conn = client.sock
        conn.settimeout(None)
        buf = b""
        while not client.closed:
            try:
                chunk = conn.recv(4096)
            except OSError:
                break
            if not chunk:
                break
            buf += chunk
            while len(buf) >= 2:
                opcode = buf[0] & 0x0F
                masked = buf[1] & 0x80
                length = buf[1] & 0x7F
                offset = 2
                if length == 126:
                    if len(buf) < 4:
                        break
                    length = struct.unpack(">H", buf[2:4])[0]
                    offset = 4
                elif length == 127:
                    if len(buf) < 10:
                        break
                    length = struct.unpack(">Q", buf[2:10])[0]
                    offset = 10
                mask_key = b""
                if masked:
                    if len(buf) < offset + 4:
                        break
                    mask_key = buf[offset:offset + 4]
                    offset += 4
                if len(buf) < offset + length:
                    break
                payload = bytearray(buf[offset:offset + length])
                buf = buf[offset + length:]
                if masked:
                    for i in range(length):
                        payload[i] ^= mask_key[i % 4]
                if opcode == 0x8:
                    self.drop(client)
                    return
                if opcode == 0x1:
                    self.on_text(payload.decode("utf-8", "replace"))
        self.drop(client)


class PageHandler(http.server.BaseHTTPRequestHandler):
    page = b""
    page_lock = threading.Lock()

    def do_GET(self):  # noqa: N802 - http.server API
        path = urllib.parse.urlparse(self.path).path
        if path in ("/", "/index.html"):
            with self.page_lock:
                body = self.page
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            # Same reason as the firmware: an older viewer script must never win.
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(body)
            return
        if path == "/api/mirror/status":
            body = b'{"state":"running","clients":1,"usb":true}'
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        self.send_error(404)

    def log_message(self, format, *args):  # noqa: A002 - keep console quiet
        pass


class FrameParser:
    """Splits the CDC stream into ALM frames and text lines.

    A binary frame always starts with `MSF1`; text lines always end with a
    newline. Frames are parsed first so a payload byte that happens to be 0x0A
    can never be mistaken for a log line.
    """

    def __init__(self, on_frame, on_line, on_corrupt):
        self.buf = bytearray()
        self.on_frame = on_frame
        self.on_line = on_line
        self.on_corrupt = on_corrupt

    def feed(self, chunk: bytes) -> None:
        self.buf += chunk
        while self.buf:
            if self.buf.startswith(FRAME_MAGIC):
                if len(self.buf) < 8:
                    return
                length = struct.unpack("<I", self.buf[4:8])[0]
                if length == 0 or length > 200000:
                    del self.buf[:1]  # bogus header, resync
                    continue
                total = 8 + length + 2
                if len(self.buf) < total:
                    return
                payload = bytes(self.buf[8:8 + length])
                crc = struct.unpack("<H", self.buf[8 + length:total])[0]
                del self.buf[:total]
                if crc != crc16(payload) or not payload.startswith(ALM_MAGIC):
                    # Log text interleaved with the frame, or a truncated send.
                    self.on_corrupt()
                    continue
                self.on_frame(payload)
                continue

            newline = self.buf.find(b"\n")
            if newline >= 0:
                line = bytes(self.buf[:newline])
                del self.buf[:newline + 1]
                text = line.decode("utf-8", "replace").strip()
                if text:
                    self.on_line(text)
                continue

            if FRAME_MAGIC.startswith(bytes(self.buf)):
                return  # partial magic, wait for the rest
            if len(self.buf) > 512:
                # A partial line that never terminates; drop its head but keep a
                # tail in case a magic straddles the cut.
                del self.buf[:len(self.buf) - 3]
            return


def crc16(data: bytes) -> int:
    crc = 0xFFFF
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def fetch_page(link: SerialLink) -> bytes:
    link.command("MIRROR PAGE")
    deadline = time.time() + 10.0
    while time.time() < deadline:
        line = link.read_line(1.0)
        if line.startswith("MIRROR PAGE "):
            length = int(line.split()[2])
            body = link.read_exact(length, 10.0)
            if len(body) != length:
                raise RuntimeError(f"page truncated: {len(body)}/{length}")
            return body
        if line:
            log(f"device: {line}")
    raise RuntimeError("device did not answer MIRROR PAGE")


def main() -> int:
    parser = argparse.ArgumentParser(description="AirLinkOS USB screen mirror bridge")
    parser.add_argument("--port", default="", help="serial port (default: auto-detect)")
    parser.add_argument("--http-port", type=int, default=8080)
    parser.add_argument("--ws-port", type=int, default=8081)
    parser.add_argument("--open", action="store_true", help="open the viewer page in a browser")
    parser.add_argument("--debug-status", action="store_true", help="poll MIRROR STATUS every 5 s")
    args = parser.parse_args()

    port = args.port or find_port()
    if not port:
        log("no USB serial port found; plug the device in")
        return 1

    link = SerialLink(port)
    try:
        PageHandler.page = fetch_page(link)
        log(f"viewer page: {len(PageHandler.page)} bytes")

        hub = WsHub(args.ws_port, on_text=lambda text: forward_text(link, text),
                    on_connect=lambda: link.command("MIRROR RESYNC"))
        threading.Thread(target=hub.serve_forever, daemon=True).start()

        httpd = http.server.ThreadingHTTPServer(("127.0.0.1", args.http_port), PageHandler)
        httpd.daemon_threads = True
        threading.Thread(target=httpd.serve_forever, daemon=True).start()

        link.command("MIRROR ON")
        deadline = time.time() + 5.0
        ready = False
        while time.time() < deadline:
            line = link.read_line(1.0)
            if line.startswith("MIRROR READY"):
                log(f"device streaming at {line.split(' ', 2)[2]}")
                ready = True
                break
            if line.startswith("MIRROR ERR") or line.startswith("MIRROR BUSY"):
                log(f"device refused: {line}")
                return 1
            if line:
                log(f"device: {line}")
        if not ready:
            log("no MIRROR READY; is the device running this firmware?")
            return 1

        parser = FrameParser(on_frame=lambda payload: broadcast_and_count(hub, payload),
                             on_line=lambda text: log(f"device: {text}"),
                             on_corrupt=lambda: request_resync(link))

        url = f"http://127.0.0.1:{args.http_port}/"
        log(f"open {url}")
        if args.open:
            webbrowser.open(url)

        last_status = 0.0
        while True:
            chunk = link.read(0.5)
            if chunk:
                parser.feed(chunk)
            if args.debug_status and time.time() - last_status > 5.0:
                last_status = time.time()
                link.command("MIRROR STATUS")
    except KeyboardInterrupt:
        log("stopping")
    finally:
        try:
            link.command("MIRROR OFF")
            time.sleep(0.2)
        except OSError:
            pass
        link.close()
    return 0


def forward_text(link: SerialLink, text: str) -> None:
    if text in ("PING", "") or text.startswith("AUTH"):
        return
    if text.startswith("KEY ") or text.startswith("TAP "):
        link.command(f"MIRROR {text}")
    elif text == "HOME":
        link.command("MIRROR KEY HOME")


_last_resync = [0.0]
_frame_count = [0]


def broadcast_and_count(hub: WsHub, payload: bytes) -> None:
    _frame_count[0] += 1
    if _frame_count[0] == 1 or _frame_count[0] % 20 == 0:
        log(f"frame #{_frame_count[0]} ({len(payload)} bytes)")
    hub.broadcast(payload)


def request_resync(link: SerialLink) -> None:
    """Ask for a full frame after a damaged one, at most once per second."""
    now = time.time()
    if now - _last_resync[0] < 1.0:
        return
    _last_resync[0] = now
    log("frame damaged by interleaved log text; asking for a full frame")
    link.command("MIRROR RESYNC")


if __name__ == "__main__":
    sys.exit(main())
