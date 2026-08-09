#!/usr/bin/env python3
"""tether.py — give the cube internet through this computer's USB cable.

Some networks the cube cannot join on its own: an office with 802.1X, a captive
portal it cannot click through, a guest network that will not take a device
without a browser. This script is the way round that. The cube asks, over the
serial line, for HTTP requests to be performed; this performs them and streams
the answers back. Nothing about the cube's own WiFi needs to work.

    pip install pyserial requests
    python3 tools/tether.py --port /dev/cu.usbserial-110

The wire format is src/SerialFrame.h — SLIP framing with a CRC, so frames and
the cube's ordinary debug log share the one UART without either corrupting the
other. Anything that is not a valid frame is printed as log text, which is
exactly what you want when something goes wrong.

Run the framing checks with:
    python3 tools/tether.py --selftest
which round-trips this implementation against fixtures produced by the C++ one,
so the two cannot quietly disagree about endianness or the CRC.
"""

import argparse
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


# ---------------------------------------------------------------------------
def serve(port: str, baud: int, verbose: bool):
    import serial  # pyserial
    import requests

    ser = serial.Serial(port, baud, timeout=0.05)
    print(f"tether: listening on {port} at {baud}", file=sys.stderr)

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
            try:
                r = requests.request(
                    "POST" if method == METHOD_POST else "GET",
                    url, headers=headers, data=body or None,
                    stream=True, timeout=20,
                )
                total = int(r.headers.get("content-length") or 0)
                send(HTTP_STATUS, fid, struct.pack("<HI", r.status_code, total))
                for chunk in r.iter_content(CHUNK):
                    if chunk:
                        send(HTTP_DATA, fid, chunk)
                send(HTTP_END, fid)
                if verbose:
                    print(f"tether: #{fid} -> {r.status_code}", file=sys.stderr)
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
                (HELLO, 1, b"smalltv-mod"),
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

    print("\n-------------------------------------------------------------")
    if failures:
        print(f"{failures} check(s) FAILED")
        return 1
    print("all checks passed")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", help="serial port, e.g. /dev/cu.usbserial-110")
    ap.add_argument("--baud", type=int, default=460800,
                    help="must match the firmware (default 460800)")
    ap.add_argument("-v", "--verbose", action="store_true",
                    help="log every request the cube makes")
    ap.add_argument("--selftest", action="store_true",
                    help="check the framing against the C++ implementation and exit")
    a = ap.parse_args()

    if a.selftest:
        return selftest()
    if not a.port:
        ap.error("--port is required (or use --selftest)")
    try:
        serve(a.port, a.baud, a.verbose)
    except KeyboardInterrupt:
        return 0
    return 0


if __name__ == "__main__":
    sys.exit(main())
