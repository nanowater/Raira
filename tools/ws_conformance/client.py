#!/usr/bin/env python3
"""Talks to the harness server with an independent, hand-written WebSocket client and asserts what RFC 6455 requires:
the Sec-WebSocket-Accept of the RFC's own example, text and binary echo (also 64-bit lengths), ping/pong, fragmentation,
close, refusing another path and surviving garbage. Usage: client.py <port>. Exit code 0 = all passed."""
import os
import socket
import struct
import sys
import time

PORT = int(sys.argv[1])
FAILED = []


def check(name, ok, detail=""):
    print(("ok    " if ok else "FAIL  ") + name + (f"  ({detail})" if detail and not ok else ""))
    if not ok:
        FAILED.append(name)


def frame(op, payload, fin=True):
    n, mask = len(payload), os.urandom(4)
    head = struct.pack("!BB", (0x80 if fin else 0) | op, 0x80 | n) if n < 126 else \
        struct.pack("!BBH", (0x80 if fin else 0) | op, 0x80 | 126, n) if n < 65536 else \
        struct.pack("!BBQ", (0x80 if fin else 0) | op, 0x80 | 127, n)
    return head + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(payload))


def recv_exact(s, n):
    d = b""
    while len(d) < n:
        c = s.recv(n - len(d))
        if not c:
            raise EOFError
        d += c
    return d


def read_frame(s):
    b0, b1 = recv_exact(s, 2)
    n = b1 & 0x7F
    if n == 126:
        n = struct.unpack("!H", recv_exact(s, 2))[0]
    elif n == 127:
        n = struct.unpack("!Q", recv_exact(s, 8))[0]
    return b0 & 0x0F, recv_exact(s, n)


def connect(path="/source"):
    s = socket.create_connection(("127.0.0.1", PORT), timeout=3)
    s.sendall(f"GET {path} HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
              "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n".encode())
    head = b""
    while b"\r\n\r\n" not in head:
        head += s.recv(1)
    return s, head.decode()


s, head = connect()
check("handshake answers 101 with the RFC's example accept value", " 101 " in head.split("\r\n")[0] and "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=" in head, head)
s.sendall(frame(1, b'{"t":"hello"}'))
check("text frame is echoed", read_frame(s) == (1, b'echo:{"t":"hello"}'))
big = os.urandom(70000)
s.sendall(frame(2, big))
op, data = read_frame(s)
check("70 KB binary frame (64-bit length) comes back identical", op == 2 and data == big)
s.sendall(frame(1, b"x" * 300))
op, data = read_frame(s)
check("300 byte text frame (16-bit length)", op == 1 and len(data) == 305)
s.sendall(frame(9, b"hi"))
check("ping is answered with a pong carrying the same data", read_frame(s) == (10, b"hi"))
s.sendall(frame(1, b"part1-", fin=False) + frame(0, b"part2"))
check("a fragmented text message is reassembled", read_frame(s) == (1, b"echo:part1-part2"))
s.sendall(frame(8, struct.pack("!H", 1000)))
check("a close frame is answered with a close frame", read_frame(s)[0] == 8)
s.close()

s2, head2 = connect("/other")
time.sleep(0.4)
s2.settimeout(1)
try:
    tail = s2.recv(10)
except socket.timeout:
    tail = None
check("another path is not served (the connection is closed)", tail == b"" or " 403 " in head2, f"{head2!r} {tail!r}")

s3 = socket.create_connection(("127.0.0.1", PORT), timeout=3)
s3.sendall(b"GARBAGE\r\n\r\n")
time.sleep(0.3)
s3.settimeout(1)
try:
    reply = s3.recv(100)
except socket.timeout:
    reply = None
check("garbage input is refused, not served", reply is not None and not reply.startswith(b"HTTP/1.1 101"), reply)

s4, _ = connect()
s4.sendall(frame(1, b"still alive?"))
check("the server still works after all of that", read_frame(s4) == (1, b"echo:still alive?"))
s4.close()

print(f"\n{len(FAILED)} failed" if FAILED else "\nall passed")
sys.exit(1 if FAILED else 0)
