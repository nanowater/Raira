#!/usr/bin/env python3
"""A fake hub source (docs/HUB_PROTOCOL.md): connects to a device (lvglsim -S <port>), authenticates or pairs and plays a
scenario of players and states, so the device side can be checked without a real player. Needs only the Python standard library.

  python3 -I tools/fake_source.py --port 18399 --scenario playing
  python3 -I tools/fake_source.py --port 18399 --pair 123456          # pairs with the code shown on the device, prints the secret
  python3 -I tools/fake_source.py --port 18399 --scenario-file my.json

What the device sends back (cmd, cover_req, lyrics_req, focus) is printed; cmd is acknowledged, cover_req answered with
cover_err, lyrics_req (only with --features lyrics) with "not found".

A step may carry "expect_focus": the player id the device must be showing (it announces its choice with a `focus` message) when
the step ends; a mismatch makes the exit code 1. That is how the Automatic rule is checked without looking at the screen.

Exit code: 0 when the scenario ran to its end and every expectation held, 1 when the connection, the authentication or an
expectation failed.
"""
import argparse
import json
import os
import select
import socket
import struct
import sys
import time


def frame(op, payload=b""):
    n = len(payload)
    mask = os.urandom(4)
    if n < 126:
        head = struct.pack("!BB", 0x80 | op, 0x80 | n)
    elif n < 65536:
        head = struct.pack("!BBH", 0x80 | op, 0x80 | 126, n)
    else:
        head = struct.pack("!BBQ", 0x80 | op, 0x80 | 127, n)
    return head + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(payload))


class Ws:
    def __init__(self, host, port, path="/source"):
        self.s = socket.create_connection((host, port), timeout=3)
        self.s.sendall(
            f"GET {path} HTTP/1.1\r\nHost: {host}:{port}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
            "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n".encode())
        head = b""
        while b"\r\n\r\n" not in head:
            c = self.s.recv(1)
            if not c:
                raise ConnectionError("the device closed the connection during the handshake")
            head += c
        if b" 101 " not in head.split(b"\r\n")[0]:
            raise ConnectionError("handshake refused: " + head.split(b"\r\n")[0].decode())
        self.buf = b""
        self.closed = False

    def send(self, obj):
        self.s.sendall(frame(1, json.dumps(obj).encode()))

    def _read_frames(self):
        out = []
        while len(self.buf) >= 2:
            b0, b1 = self.buf[0], self.buf[1]
            n, off = b1 & 0x7F, 2
            if n == 126:
                if len(self.buf) < 4:
                    break
                n, off = struct.unpack("!H", self.buf[2:4])[0], 4
            elif n == 127:
                if len(self.buf) < 10:
                    break
                n, off = struct.unpack("!Q", self.buf[2:10])[0], 10
            if len(self.buf) < off + n:
                break
            payload, self.buf = self.buf[off:off + n], self.buf[off + n:]
            op = b0 & 0x0F
            if op == 9:
                self.s.sendall(frame(10, payload))  # ping -> pong
            elif op == 8:
                self.closed = True
            elif op in (1, 2):
                out.append((op, payload))
        return out

    def poll(self, seconds):
        """Messages that arrive within `seconds` (text ones decoded to dicts)."""
        msgs, end = [], time.time() + seconds
        while time.time() < end and not self.closed:
            r, _, _ = select.select([self.s], [], [], max(0.0, min(0.2, end - time.time())))
            if not r:
                continue
            data = self.s.recv(65536)
            if not data:
                self.closed = True
                break
            self.buf += data
            for op, payload in self._read_frames():
                msgs.append(json.loads(payload) if op == 1 else {"t": "<binary>", "bytes": len(payload)})
        return msgs


def players(selected, *ps):
    return {"t": "players", "selected": selected, "players": [dict(id=i, name=n, service=s, status=st) for i, n, s, st in ps]}


def state(player, title, status, pos, artists=("Fake Artist",), album="Fake Album", dur=245000, caps=("seek", "prev_next"), **extra):
    m = {"t": "state", "player": player, "status": status,
         "track": {"id": player + ":" + title, "title": title, "artists": list(artists), "album": album, "duration_ms": dur, "cover": None},
         "position_ms": pos, "rate": 1.0, "volume": 1.0, "shuffle": False, "repeat": "off", "quality": None, "capabilities": list(caps)}
    m.update(extra)
    return m


# Built-in scenarios: a list of steps {"send": [messages], "wait": seconds}.
FEEL = dict(artists=("Hitsujibungaku",), album="Feel")  # a track LRCLIB has synced lyrics for; line 3 starts at 31 790 ms
SCENARIOS = {
    "playing": [{"send": [players("p", ("p", "Fake player", "other", "playing")), state("p", "Feel", "playing", 30000, **FEEL)], "wait": 30,
                 "expect_focus": "p"}],
    "idle": [{"send": [players(None)], "wait": 8}],
    # paused 150 ms either side of a lyric line boundary: the highlight must flip between the two (needs network for lyrics)
    "boundary": [{"send": [players("p", ("p", "Fake player", "other", "paused")), state("p", "Feel", "paused", pos, **FEEL)], "wait": w}
                 for pos, w in [(31640, 7), (31940, 4), (31640, 4), (31940, 3)]],
    # the source's own pick must win over a newer playing player (docs/HUB_PROTOCOL.md, Automatic)
    "pick": [{"send": [players(sel, ("tidal", "tidal-hifi", "tidal", "paused"), ("web", "Firefox", "browser", "playing")),
                       state("tidal", "TIDAL song (paused)", "paused", 40000),
                       state("web", "Browser video (playing)", "playing", 40000)], "wait": 5, "expect_focus": sel}
             for sel in ("tidal", "web", "tidal")],
}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, required=True)
    ap.add_argument("--source-id", default="fake")
    ap.add_argument("--name", default="Fake PC")
    ap.add_argument("--secret", default="s3cr3t", help="the pairing secret the device has stored for this source id (see seed_device.py)")
    ap.add_argument("--pair", metavar="CODE", help="pair with this code instead of authenticating; the new secret is printed")
    ap.add_argument("--features", default="", help="comma separated: cover, lyrics")
    ap.add_argument("--scenario", default="playing", choices=sorted(SCENARIOS))
    ap.add_argument("--scenario-file", help="JSON: a list of steps {\"send\": [messages], \"wait\": seconds}")
    ap.add_argument("--linger", type=float, default=0, help="stay connected this many seconds after the scenario")
    a = ap.parse_args()

    steps = json.load(open(a.scenario_file)) if a.scenario_file else SCENARIOS[a.scenario]
    try:
        ws = Ws(a.host, a.port)
    except OSError as e:
        print("cannot connect:", e, file=sys.stderr)
        return 1

    features = [f for f in a.features.split(",") if f]
    ws.send({"t": "hello", "proto": 1, "source_id": a.source_id, "name": a.name, "os": "linux", "features": features})
    ws.send({"t": "pair", "code": a.pair} if a.pair else {"t": "auth", "secret": a.secret})

    ready = False
    deadline = time.time() + 5
    while not ready and time.time() < deadline and not ws.closed:
        for m in ws.poll(0.3):
            if m["t"] in ("ready", "paired"):
                ready = True
                if m["t"] == "paired":
                    print("paired, secret:", m["secret"])
            elif m["t"] in ("auth_failed", "pair_failed", "pairing_required") and not (m["t"] == "pairing_required" and a.pair):
                print("not accepted:", json.dumps(m), file=sys.stderr)
                return 1
    if not ready:
        print("the device did not accept this source in time", file=sys.stderr)
        return 1
    print("ready")

    focus = [None]  # what the device last said it is showing

    def handle(msgs):
        for m in msgs:
            print("<-", json.dumps(m, ensure_ascii=False))
            if m["t"] == "focus":
                focus[0] = m.get("player")
            if m["t"] == "cmd":
                ws.send({"t": "ack", "id": m.get("id"), "ok": True})
            elif m["t"] == "cover_req":
                ws.send({"t": "cover_err", "id": m.get("id")})
            elif m["t"] == "lyrics_req":
                ws.send({"t": "lyrics", "id": m.get("id"), "found": False})

    failures = 0
    for i, step in enumerate(steps):
        for msg in step.get("send", []):
            ws.send(msg)
        print(f"step {i + 1}/{len(steps)} sent, waiting {step.get('wait', 1)} s", flush=True)
        handle(ws.poll(step.get("wait", 1)))
        if ws.closed:
            print("the device closed the connection", file=sys.stderr)
            return 1
        if "expect_focus" in step:
            ok = focus[0] == step["expect_focus"]
            failures += not ok
            print(("check ok:   " if ok else "CHECK FAILED: ") + f"the device shows {focus[0]!r}, expected {step['expect_focus']!r}")
    handle(ws.poll(a.linger))
    ws.s.close()
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
