#!/usr/bin/env python3
"""tether.py — give the cube internet through this computer's USB cable.

Some networks the cube cannot join on its own: an office with 802.1X, a captive
portal it cannot click through, a guest network that will not take a device
without a browser. Plug the cube into this computer and run:

    python3 tether.py

That is the whole procedure. No port to find, no arguments, and on macOS and
Linux nothing to install — it drives the serial port through the standard
library. Windows needs `pip install pyserial`, which it will tell you.

It finds the cube by listening: every candidate serial port is opened and
watched for the announcement the firmware sends every couple of seconds, and
the one that answers is the cube. Unplug it and the script goes back to
looking, so moving the cable between machines needs nothing from you.

The wire format is src/SerialFrame.h — SLIP framing with a CRC, so frames and
the cube's ordinary debug log share the one UART without either corrupting the
other. Log lines are printed as they arrive, which is what you want when
something goes wrong.

    python3 tether.py --selftest    check the framing against the C++ side
"""


import argparse
import glob
import os
import struct
import sys
import time

END, ESC, ESC_END, ESC_ESC = 0xC0, 0xDB, 0xDC, 0xDD
VERSION = 1
HEADER = 6
MAX_PAYLOAD = 1024

HELLO, HELLO_ACK = 0x01, 0x02
HTTP_REQ, HTTP_STATUS, HTTP_DATA, HTTP_END, HTTP_ERR = 0x10, 0x11, 0x12, 0x13, 0x14
TIME, LOG = 0x20, 0x30

METHOD_GET, METHOD_POST = 0, 1

# Body chunk sent per frame. Smaller than MAX_PAYLOAD so a chunk plus its
# escaping never approaches the cube's receive buffer.
CHUNK = 512


def crc16(data: bytes, crc: int = 0xFFFF) -> int:
    """CRC-16/CCITT-FALSE — must match sfCrc16 in src/SerialFrame.h."""
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def encode(ftype: int, fid: int, payload: bytes = b"") -> bytes:
    if len(payload) > MAX_PAYLOAD:
        raise ValueError(f"payload {len(payload)} over the {MAX_PAYLOAD} cap")
    header = struct.pack("<BBHH", VERSION, ftype, fid, len(payload))
    body = header + payload + struct.pack(">H", crc16(header + payload))
    out = bytearray([END])
    for b in body:
        if b == END:
            out += bytes([ESC, ESC_END])
        elif b == ESC:
            out += bytes([ESC, ESC_ESC])
        else:
            out.append(b)
    out.append(END)
    return bytes(out)


class Decoder:
    """Incremental decoder. Yields (type, id, payload) and prints the rest."""

    def __init__(self, on_log=None):
        self.buf = bytearray()
        self.in_frame = False
        self.escaped = False
        self.overrun = False
        self.text = bytearray()
        self.on_log = on_log or (lambda s: None)

    def feed(self, data: bytes):
        for b in data:
            frame = self._byte(b)
            if frame is not None:
                yield frame

    def _byte(self, b: int):
        if b == END:
            frame = None
            if self.in_frame and not self.overrun:
                frame = self._finish()
            if frame is None and self.buf:
                # Not a frame after all: it was log text between delimiters.
                self._emit_text(bytes(self.buf))
            self.in_frame = True
            self.buf.clear()
            self.escaped = False
            self.overrun = False
            return frame

        if not self.in_frame:
            self._emit_text(bytes([b]))
            return None

        if self.escaped:
            self.escaped = False
            if b == ESC_END:
                b = END
            elif b == ESC_ESC:
                b = ESC
            else:
                self.overrun = True
                return None
        elif b == ESC:
            self.escaped = True
            return None

        if len(self.buf) >= HEADER + MAX_PAYLOAD + 2:
            self.overrun = True
            return None
        self.buf.append(b)
        return None

    def _emit_text(self, chunk: bytes):
        self.text += chunk
        while b"\n" in self.text:
            line, _, rest = bytes(self.text).partition(b"\n")
            self.text = bytearray(rest)
            s = line.decode("utf-8", "replace").rstrip("\r")
            if s:
                self.on_log(s)

    def _finish(self):
        if len(self.buf) < HEADER + 2:
            return None
        ver, ftype, fid, plen = struct.unpack("<BBHH", bytes(self.buf[:HEADER]))
        if ver != VERSION or len(self.buf) != HEADER + plen + 2:
            return None
        want = struct.unpack(">H", bytes(self.buf[-2:]))[0]
        if crc16(bytes(self.buf[: HEADER + plen])) != want:
            return None
        return (ftype, fid, bytes(self.buf[HEADER : HEADER + plen]))


def parse_request(payload: bytes):
    """method, url, headers dict, body — mirrors what the firmware packs."""
    o = 0
    method = payload[o]; o += 1
    (url_len,) = struct.unpack_from("<H", payload, o); o += 2
    url = payload[o : o + url_len].decode(); o += url_len
    (hdr_len,) = struct.unpack_from("<H", payload, o); o += 2
    raw = payload[o : o + hdr_len].decode(); o += hdr_len
    (body_len,) = struct.unpack_from("<H", payload, o); o += 2
    body = payload[o : o + body_len]
    headers = {}
    for line in raw.split("\n"):
        if ":" in line:
            k, _, v = line.partition(":")
            headers[k.strip()] = v.strip()
    return method, url, headers, body


class Port:
    """A serial port, without requiring pyserial where the platform will do.

    macOS and Linux can set a raw 115200 line discipline through termios, which
    is in the standard library — so the common case is "download one file and
    run it" rather than "install a package first". Windows has no such API and
    falls back to pyserial.
    """

    def __init__(self, path: str, baud: int):
        self.path = path
        self._ser = None
        self._fd = None
        if os.name == "nt":
            try:
                import serial
            except ImportError:
                raise SystemExit("tether: Windows needs pyserial — pip install pyserial")
            self._ser = serial.Serial(path, baud, timeout=0.05)
            return

        import termios
        self._fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        attrs = termios.tcgetattr(self._fd)
        iflag, oflag, cflag, lflag, ispeed, ospeed, cc = attrs
        speed = getattr(termios, f"B{baud}", None)
        if speed is None:
            os.close(self._fd)
            raise SystemExit(f"tether: this platform cannot do {baud} baud")
        # Raw: no echo, no translation, no flow control. Anything else would
        # mangle binary frames.
        iflag = 0
        oflag = 0
        cflag = termios.CS8 | termios.CREAD | termios.CLOCAL
        lflag = 0
        cc = list(cc)
        cc[termios.VMIN] = 0
        cc[termios.VTIME] = 0
        termios.tcsetattr(self._fd, termios.TCSANOW,
                          [iflag, oflag, cflag, lflag, speed, speed, cc])

    def read(self, n: int) -> bytes:
        if self._ser is not None:
            return self._ser.read(n)
        try:
            return os.read(self._fd, n)
        except BlockingIOError:
            return b""
        except OSError:
            raise ConnectionError(f"{self.path} went away")

    def write(self, data: bytes):
        if self._ser is not None:
            self._ser.write(data)
            return
        try:
            while data:
                data = data[os.write(self._fd, data):]
        except OSError:
            raise ConnectionError(f"{self.path} went away")

    def close(self):
        try:
            if self._ser is not None:
                self._ser.close()
            elif self._fd is not None:
                os.close(self._fd)
        except Exception:                                # noqa: BLE001
            pass


def candidate_ports():
    """Serial devices worth listening to, most likely first."""
    if os.name == "nt":
        try:
            from serial.tools import list_ports
            return [p.device for p in list_ports.comports()]
        except ImportError:
            return [f"COM{i}" for i in range(1, 33)]
    # cu.* rather than tty.* on macOS: tty.* blocks on open waiting for carrier.
    pats = ["/dev/cu.usbserial*", "/dev/cu.usbmodem*", "/dev/cu.wchusbserial*",
            "/dev/cu.SLAB_USBtoUART*", "/dev/ttyUSB*", "/dev/ttyACM*"]
    out = []
    for p in pats:
        out += sorted(glob.glob(p))
    # Never grab a Bluetooth or debug console pseudo-port.
    return [p for p in out if "Bluetooth" not in p and "debug" not in p]


def find_cube(baud: int, verbose: bool, listen_s: float = 3.0):
    """Open each candidate and wait for the firmware's hello. Returns a Port."""
    ports = candidate_ports()
    if not ports:
        return None
    for path in ports:
        try:
            p = Port(path, baud)
        except (SystemExit, Exception) as e:             # noqa: BLE001
            if isinstance(e, SystemExit):
                raise
            if verbose:
                print(f"tether: {path}: {e}", file=sys.stderr)
            continue
        dec = Decoder()
        deadline = time.time() + listen_s
        # The firmware announces itself every 2 s; give it more than one turn.
        while time.time() < deadline:
            data = p.read(4096)
            if not data:
                time.sleep(0.02)
                continue
            for ftype, fid, payload in dec.feed(data):
                if ftype == HELLO:
                    who = payload.decode("utf-8", "replace")
                    print(f"tether: found {who} on {path}", file=sys.stderr)
                    return p
        if verbose:
            print(f"tether: nothing on {path}", file=sys.stderr)
        p.close()
    return None


# ---------------------------------------------------------------------------
def serve_port(ser, verbose: bool):
    from urllib import request as urlreq
    from urllib import error as urlerr

    def log(s):
        print(f"  cube | {s}", file=sys.stderr)

    dec = Decoder(on_log=log)

    def send(ftype, fid, payload=b""):
        ser.write(encode(ftype, fid, payload))

    def send_time():
        send(TIME, 0, struct.pack("<q", int(time.time())))

    while True:
        data = ser.read(4096)
        if not data:
            time.sleep(0.01)
            continue
        for ftype, fid, payload in dec.feed(data):
            if ftype == HELLO:
                who = payload.decode("utf-8", "replace")
                print(f"tether: cube says hello ({who})", file=sys.stderr)
                send(HELLO_ACK, fid)
                send_time()
                continue

            if ftype != HTTP_REQ:
                continue

            try:
                method, url, headers, body = parse_request(payload)
            except Exception as e:                       # noqa: BLE001
                send(HTTP_ERR, fid, f"bad request frame: {e}".encode()[:200])
                continue

            if verbose:
                print(f"tether: #{fid} {'POST' if method else 'GET'} {url}",
                      file=sys.stderr)
            # urllib rather than requests: one less thing to install, and the
            # streaming is good enough for bodies this size.
            try:
                req = urlreq.Request(
                    url, data=body or None, headers=headers,
                    method="POST" if method == METHOD_POST else "GET")
                with urlreq.urlopen(req, timeout=20) as r:
                    total = int(r.headers.get("content-length") or 0)
                    send(HTTP_STATUS, fid, struct.pack("<HI", r.status, total))
                    while True:
                        chunk = r.read(CHUNK)
                        if not chunk:
                            break
                        send(HTTP_DATA, fid, chunk)
                    send(HTTP_END, fid)
                    if verbose:
                        print(f"tether: #{fid} -> {r.status}", file=sys.stderr)
            except urlerr.HTTPError as e:
                # A 404 or a 401 is an answer, not a failure of the tether: pass
                # the status through so the cube reports what the server said.
                body_bytes = e.read() or b""
                send(HTTP_STATUS, fid, struct.pack("<HI", e.code, len(body_bytes)))
                for i in range(0, len(body_bytes), CHUNK):
                    send(HTTP_DATA, fid, body_bytes[i:i + CHUNK])
                send(HTTP_END, fid)
                if verbose:
                    print(f"tether: #{fid} -> {e.code}", file=sys.stderr)
            except Exception as e:                       # noqa: BLE001
                # The cube can do nothing about the reason, but it can show it,
                # and "why is the ticker blank" is otherwise unanswerable.
                send(HTTP_ERR, fid, str(e).encode()[:200])
                if verbose:
                    print(f"tether: #{fid} failed: {e}", file=sys.stderr)


# ---------------------------------------------------------------------------
def selftest() -> int:
    """Round-trip this framing, and check it against the C++ implementation."""
    import os
    import subprocess
    import tempfile

    failures = 0

    def ck(cond, what):
        nonlocal failures
        print(f"  {'ok   ' if cond else 'FAIL '} {what}")
        if not cond:
            failures += 1

    print("--- python round trip ---------------------------------------")
    for name, payload in [
        ("empty", b""),
        ("text", b"GET https://example.com/x"),
        ("all delimiters", bytes([END]) * 40),
        ("all escapes", bytes([ESC]) * 40),
        ("binary ramp", bytes(range(256)) * 2),
        ("max payload", b"x" * MAX_PAYLOAD),
    ]:
        wire = encode(HTTP_DATA, 0x2A, payload)
        got = list(Decoder().feed(wire))
        ck(len(got) == 1 and got[0] == (HTTP_DATA, 0x2A, payload), f"{name} survives")

    print("\n--- log text is not mistaken for a frame --------------------")
    lines = []
    d = Decoder(on_log=lines.append)
    frames = list(d.feed(b"[boot] settings\n[boot] display\n"))
    ck(not frames, "plain log yields no frames")
    ck(lines == ["[boot] settings", "[boot] display"], "and is surfaced as text")

    d = Decoder(on_log=lines.append)
    mixed = b"[net] up\n" + encode(HELLO, 1, b"smalltv") + b"[net] rssi -50\n"
    frames = list(d.feed(mixed))
    ck(frames == [(HELLO, 1, b"smalltv")], "a frame between log lines is found")

    print("\n--- corruption ----------------------------------------------")
    wire = bytearray(encode(HTTP_DATA, 5, b"abcdefgh"))
    wire[6] ^= 0x01
    ck(not list(Decoder().feed(bytes(wire))), "a flipped bit is rejected")

    print("\n--- against the C++ implementation --------------------------")
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    src = os.path.join(root, "tools", "serialframe_selftest", "emit.cpp")
    with tempfile.TemporaryDirectory() as tmp:
        exe = os.path.join(tmp, "emit")
        r = subprocess.run(["g++", "-O1", "-std=c++17", "-o", exe, src],
                           capture_output=True, text=True)
        if r.returncode != 0:
            print(r.stderr)
            ck(False, "built the C++ frame emitter")
        else:
            out = subprocess.run([exe], capture_output=True).stdout
            got = list(Decoder().feed(out))
            # emit.cpp writes these three, in this order.
            want = [
                (HELLO, 1, b"smalltv-plus"),
                (HTTP_DATA, 0x1234, bytes(range(256))),
                (HTTP_END, 0xBEEF, b""),
            ]
            ck(got == want, "python decodes frames the C++ encoder produced")

            # And the other direction: the C++ decoder is given python's bytes.
            dec_src = os.path.join(root, "tools", "serialframe_selftest", "decode.cpp")
            exe2 = os.path.join(tmp, "decode")
            r2 = subprocess.run(["g++", "-O1", "-std=c++17", "-o", exe2, dec_src],
                                capture_output=True, text=True)
            if r2.returncode != 0:
                print(r2.stderr)
                ck(False, "built the C++ frame decoder")
            else:
                blob = b"".join(encode(t, i, p) for t, i, p in want)
                r3 = subprocess.run([exe2], input=blob, capture_output=True)
                expect = b"".join(
                    f"{t} {i} {len(p)}\n".encode() for t, i, p in want)
                ck(r3.stdout == expect, "C++ decodes frames python encoded")

    print("\n--- end to end over a pseudo-terminal -----------------------")
    failures += loopback(ck)

    print("\n-------------------------------------------------------------")
    if failures:
        print(f"{failures} check(s) FAILED")
        return 1
    print("all checks passed")
    return 0


def loopback(ck) -> int:
    """Drive the whole host path with a pty standing in for the cube.

    This is the half that the framing tests cannot reach: the handshake, the
    request unpacking, and a real body streaming back in frames. A pty behaves
    like a serial port closely enough that the code under test is the code that
    will run, rather than a mock of it.
    """
    import http.server
    import threading

    before = [0]

    class Quiet(http.server.BaseHTTPRequestHandler):
        def do_GET(self):                                 # noqa: N802
            body = b"x" * 1500 + bytes(range(256))        # crosses several frames
            self.send_response(200)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def do_POST(self):                                # noqa: N802
            n = int(self.headers.get("content-length") or 0)
            sent = self.rfile.read(n)
            echo = b"got:" + sent + b" auth:" + self.headers.get("Authorization", "").encode()
            self.send_response(201)
            self.send_header("Content-Length", str(len(echo)))
            self.end_headers()
            self.wfile.write(echo)

        def log_message(self, *a):                        # keep the output clean
            pass

    srv = http.server.HTTPServer(("127.0.0.1", 0), Quiet)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    base = f"http://127.0.0.1:{srv.server_port}"

    master, slave = os.openpty()
    slave_path = os.ttyname(slave)
    os.close(slave)

    port = Port(slave_path, 115200)
    stop = threading.Event()

    def run():
        try:
            serve_port(port, verbose=False)
        except Exception:                                 # noqa: BLE001
            pass
        stop.set()

    threading.Thread(target=run, daemon=True).start()

    dec = Decoder()
    got = []

    def pump(seconds=4.0):
        deadline = time.time() + seconds
        while time.time() < deadline:
            import select
            r, _, _ = select.select([master], [], [], 0.1)
            if not r:
                continue
            for f in dec.feed(os.read(master, 65536)):
                got.append(f)
        return got

    # 1. Handshake: hello in, ack and a clock back.
    os.write(master, encode(HELLO, 0, b"smalltv-plus 2.9.10"))
    pump(2.0)
    ck(any(f[0] == HELLO_ACK for f in got), "hello is acknowledged")
    times = [f for f in got if f[0] == TIME]
    ck(bool(times), "the host offers its clock unasked")
    if times:
        (epoch,) = struct.unpack("<q", times[0][2])
        ck(abs(epoch - int(time.time())) < 5, "and the clock is right")

    # 2. A GET, streamed back across several frames.
    got.clear()
    req = (bytes([METHOD_GET])
           + struct.pack("<H", len(base + "/big")) + (base + "/big").encode()
           + struct.pack("<H", 0) + struct.pack("<H", 0))
    os.write(master, encode(HTTP_REQ, 77, req))
    pump(4.0)
    status = [f for f in got if f[0] == HTTP_STATUS and f[1] == 77]
    data = [f for f in got if f[0] == HTTP_DATA and f[1] == 77]
    end = [f for f in got if f[0] == HTTP_END and f[1] == 77]
    ck(bool(status) and struct.unpack("<HI", status[0][2])[0] == 200, "GET returns 200")
    ck(len(data) > 1, "a body larger than one frame arrives in several")
    body = b"".join(f[2] for f in data)
    ck(body == b"x" * 1500 + bytes(range(256)),
       "body is byte-identical, including bytes that collide with the framing")
    ck(bool(end), "and is terminated")

    # 3. A POST with headers and a body — the Spotify token exchange shape.
    got.clear()
    hdrs = "Authorization: Basic abc123\nContent-Type: application/x-www-form-urlencoded"
    payload = b"grant_type=refresh_token"
    req = (bytes([METHOD_POST])
           + struct.pack("<H", len(base + "/tok")) + (base + "/tok").encode()
           + struct.pack("<H", len(hdrs)) + hdrs.encode()
           + struct.pack("<H", len(payload)) + payload)
    os.write(master, encode(HTTP_REQ, 78, req))
    pump(4.0)
    status = [f for f in got if f[0] == HTTP_STATUS and f[1] == 78]
    body = b"".join(f[2] for f in got if f[0] == HTTP_DATA and f[1] == 78)
    ck(bool(status) and struct.unpack("<HI", status[0][2])[0] == 201, "POST status passes through")
    ck(b"grant_type=refresh_token" in body, "the request body reached the server")
    ck(b"Basic abc123" in body, "and so did the headers")

    # 4. A URL that cannot be reached: the cube must be told why.
    got.clear()
    bad = "http://127.0.0.1:1/nope"
    req = (bytes([METHOD_GET]) + struct.pack("<H", len(bad)) + bad.encode()
           + struct.pack("<H", 0) + struct.pack("<H", 0))
    os.write(master, encode(HTTP_REQ, 79, req))
    pump(4.0)
    errs = [f for f in got if f[0] == HTTP_ERR and f[1] == 79]
    ck(bool(errs), "an unreachable host comes back as an error, not silence")

    srv.shutdown()
    os.close(master)
    return 0


def run_forever(baud: int, verbose: bool, port: str = None):
    """Find the cube, serve it, and go back to looking when the cable goes.

    Unplugging is the normal case, not an error: the cube moves between a desk
    and a bag. Neither end should need restarting for that.
    """
    announced = False
    while True:
        ser = None
        if port:
            try:
                ser = Port(port, baud)
                print(f"tether: using {port}", file=sys.stderr)
            except Exception as e:                       # noqa: BLE001
                print(f"tether: {port}: {e}", file=sys.stderr)
        else:
            ser = find_cube(baud, verbose)

        if ser is None:
            if not announced:
                print("tether: waiting for a cube... "
                      "(plug it in; make sure nothing else holds the port)",
                      file=sys.stderr)
                announced = True
            time.sleep(2)
            continue

        announced = False
        try:
            serve_port(ser, verbose)
        except ConnectionError as e:
            print(f"tether: {e}; looking again", file=sys.stderr)
        except KeyboardInterrupt:
            ser.close()
            return
        finally:
            ser.close()


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", help="skip the search and use this port")
    ap.add_argument("--baud", type=int, default=115200,
                    help="must match the firmware (default 115200)")
    ap.add_argument("-v", "--verbose", action="store_true",
                    help="log every request the cube makes")
    ap.add_argument("--selftest", action="store_true",
                    help="check the framing against the C++ implementation and exit")
    a = ap.parse_args()

    if a.selftest:
        return selftest()
    try:
        run_forever(a.baud, a.verbose, a.port)
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
