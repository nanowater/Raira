#!/usr/bin/env python3
"""A fake hub source (docs/HUB_PROTOCOL.md): connects to a device (lvglsim -S <port>), authenticates or pairs and plays a
scenario of players and states, so the device side can be checked without a real player. Needs only the Python standard library.

  python3 -I tools/fake_source.py --port 18399 --scenario playing
  python3 -I tools/fake_source.py --port 18399 --pair 123456          # pairs with the code shown on the device, prints the secret
  python3 -I tools/fake_source.py --port 18399 --scenario-file my.json

What the device sends back (cmd, cover_req, lyrics_req, focus) is printed and counted; cmd is acknowledged. With --features
cover a cover_req is answered with a small built-in JPEG, with --features lyrics a lyrics_req with eight synced lines (without the
feature the device would not ask); otherwise cover_err / "not found".

A step may carry "expect_focus": the player id the device must be showing (it announces its choice with a `focus` message) when
the step ends, and "expect_counts": {"cover_req": N, "lyrics_req": M}, how many of those requests the device has sent so far; a
mismatch makes the exit code 1. That is how the Automatic rule and the device's cache after a reconnect are checked without
looking at the screen. A step {"disconnect": S} closes the connection, stays away S seconds and connects again. A step may also carry
"lyrics_answers": ["error", "ok"], how the next lyrics_req are answered (the last entry repeats): "ok" the lyrics, "none" found:false
(there are none), "error" found:false with an error (the lookup failed; the device asks again later). Needs --features lyrics.

Exit code: 0 when the scenario ran to its end and every expectation held, 1 when the connection, the authentication or an
expectation failed.
"""
import argparse
import base64
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

    def poll(self, seconds, on_msg=None):
        """Messages that arrive within `seconds` (text ones decoded to dicts). `on_msg` is called with each one as it arrives, so
        a request can be answered at once instead of when the wait is over."""
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
                m = json.loads(payload) if op == 1 else {"t": "<binary>", "bytes": len(payload)}
                msgs.append(m)
                if on_msg:
                    on_msg(m)
        return msgs


TINY_JPEG = base64.b64decode("/9j/4AAQSkZJRgABAQAAAQABAAD/2wBDAAoHBwgHBgoICAgLCgoLDhgQDg0NDh0VFhEYIx8lJCIfIiEmKzcvJik0KSEiMEExNDk7Pj4+JS5ESUM8SDc9Pjv/2wBDAQoLCw4NDhwQEBw7KCIoOzs7Ozs7Ozs7Ozs7Ozs7Ozs7Ozs7Ozs7Ozs7Ozs7Ozs7Ozs7Ozs7Ozs7Ozs7Ozs7Ozv/wAARCABAAEADASIAAhEBAxEB/8QAFwABAQEBAAAAAAAAAAAAAAAAAAIBBf/EABUQAQEAAAAAAAAAAAAAAAAAAAAR/8QAFgEBAQEAAAAAAAAAAAAAAAAAAAUE/8QAFREBAQAAAAAAAAAAAAAAAAAAABP/2gAMAwEAAhEDEQA/AONCKhF2iZFMIqEKEUwioQoRTCKhChFUI2EYKLMWQjYQoRZCNhChFkI2EKEWwioRgosQTCKhChBMIqEKEEwioQoQVCNhGCizFkI2EKEWQjYQoRZCNhChF//Z")  # 64x64 baseline JPEG
LYRICS = [{"ms": 1000 + 4000 * i, "text": f"fake lyric line {i + 1}"} for i in range(8)] + [{"ms": 34000, "text": ""}]
# for screenshots: RAIRA_FAKE_COVER = a JPEG file, RAIRA_FAKE_LYRICS = a JSON list of {"ms", "text"} replace the built-ins
if os.environ.get("RAIRA_FAKE_COVER"):
    TINY_JPEG = open(os.environ["RAIRA_FAKE_COVER"], "rb").read()
if os.environ.get("RAIRA_FAKE_LYRICS"):
    LYRICS = json.load(open(os.environ["RAIRA_FAKE_LYRICS"]))


def players(selected, *ps):
    return {"t": "players", "selected": selected, "players": [dict(id=i, name=n, service=s, status=st) for i, n, s, st in ps]}


def state(player, title, status, pos, artists=("Fake Artist",), album="Fake Album", dur=245000, caps=("seek", "prev_next"), cover=None, **extra):
    m = {"t": "state", "player": player, "status": status,
         "track": {"id": player + ":" + title, "title": title, "artists": list(artists), "album": album, "duration_ms": dur, "cover": cover},
         "position_ms": pos, "rate": 1.0, "volume": 1.0, "shuffle": False, "repeat": "off", "quality": None, "capabilities": list(caps)}
    m.update(extra)
    return m


# Built-in scenarios: a list of steps {"send": [messages], "wait": seconds}.
FEEL = dict(artists=("Hitsujibungaku",), album="Feel")  # a track LRCLIB has synced lyrics for; line 3 starts at 31 790 ms
SCENARIOS = {
    "playing": [{"send": [players("p", ("p", "Fake player", "other", "playing")), state("p", "Feel", "playing", 30000, **FEEL)], "wait": 30,
                 "expect_focus": "p"}],
    # every control the device can show (shuffle, repeat, volume): for looking at the control row
    "controls": [{"send": [players("p", ("p", "Fake player", "other", "playing")),
                           state("p", "Feel", "playing", 30000, caps=("seek", "volume", "shuffle", "repeat", "prev_next"), **FEEL)],
                  "wait": 30, "expect_focus": "p"}],
    "idle": [{"send": [players(None)], "wait": 8}],
    # paused 150 ms either side of a lyric line boundary: the highlight must flip between the two (needs network for lyrics)
    "boundary": [{"send": [players("p", ("p", "Fake player", "other", "paused")), state("p", "Feel", "paused", pos, **FEEL)], "wait": w}
                 for pos, w in [(31640, 7), (31940, 4), (31640, 4), (31940, 3)]],
    # the connection drops and comes back with the same track: the device must blank its screen meanwhile (look at the shots) and
    # show the cover and lyrics again WITHOUT asking for them once more; with another track it must ask again. Needs --features cover,lyrics
    "reconnect": [
        {"send": [players("p", ("p", "Fake player", "other", "playing")), state("p", "Song A", "playing", 20000, cover="a")], "wait": 7,
         "expect_focus": "p", "expect_counts": {"cover_req": 1, "lyrics_req": 1}},
        {"disconnect": 5},
        {"send": [players("p", ("p", "Fake player", "other", "playing")), state("p", "Song A", "playing", 28000, cover="a")], "wait": 4,
         "expect_focus": "p", "expect_counts": {"cover_req": 1, "lyrics_req": 1}},
        {"disconnect": 5},
        {"send": [players("p", ("p", "Fake player", "other", "playing")), state("p", "Song B", "playing", 5000, cover="b")], "wait": 5,
         "expect_focus": "p", "expect_counts": {"cover_req": 2, "lyrics_req": 2}},
    ],
    # a lookup that FAILED is asked again (8 s later), one that found nothing is not; a new track ends the retrying. Needs --features lyrics
    "lyrics_retry": [
        {"lyrics_answers": ["error", "ok"],
         "send": [players("p", ("p", "Fake player", "other", "playing")), state("p", "Song A", "playing", 20000)], "wait": 4,
         "expect_counts": {"lyrics_req": 1}},
        {"wait": 8, "expect_counts": {"lyrics_req": 2}},
        {"wait": 12, "expect_counts": {"lyrics_req": 2}},  # found: no more requests
    ],
    "lyrics_none": [
        {"lyrics_answers": ["none"],
         "send": [players("p", ("p", "Fake player", "other", "playing")), state("p", "Song A", "playing", 20000)], "wait": 4,
         "expect_counts": {"lyrics_req": 1}},
        {"wait": 14, "expect_counts": {"lyrics_req": 1}},  # nothing there: not asked again
    ],
    "lyrics_retry_track_change": [
        {"lyrics_answers": ["error"],
         "send": [players("p", ("p", "Fake player", "other", "playing")), state("p", "Song A", "playing", 20000)], "wait": 3,
         "expect_counts": {"lyrics_req": 1}},
        {"lyrics_answers": ["ok"], "send": [state("p", "Song B", "playing", 5000)], "wait": 4, "expect_counts": {"lyrics_req": 2}},
        {"wait": 12, "expect_counts": {"lyrics_req": 2}},  # Song A's retry (8 s) must not fire any more
    ],
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
    features = [f for f in a.features.split(",") if f]
    counts = {"cover_req": 0, "lyrics_req": 0}  # what the device asked for, over all connections of this run
    focus = [None]                              # what the device last said it is showing
    answers = [[], 0]                           # the "lyrics_answers" of the current step and how many were used
    session = [None]

    def open_session(pair_code=None):
        """Connect, say hello, authenticate (or pair) and wait until the device accepts. None when it does not."""
        try:
            ws = Ws(a.host, a.port)
        except OSError as e:
            print("cannot connect:", e, file=sys.stderr)
            return None
        ws.send({"t": "hello", "proto": 1, "source_id": a.source_id, "name": a.name, "os": "linux", "features": features})
        ws.send({"t": "pair", "code": pair_code} if pair_code else {"t": "auth", "secret": a.secret})
        deadline = time.time() + 5
        while time.time() < deadline and not ws.closed:
            for m in ws.poll(0.3):
                if m["t"] in ("ready", "paired"):
                    if m["t"] == "paired":
                        print("paired, secret:", m["secret"])
                    return ws
                if m["t"] in ("auth_failed", "pair_failed") or (m["t"] == "pairing_required" and not pair_code):
                    print("not accepted:", json.dumps(m), file=sys.stderr)
                    return None
        print("the device did not accept this source in time", file=sys.stderr)
        return None

    def handle(m):
        """One message from the device, answered as soon as it arrives."""
        ws = session[0]
        print("<-", json.dumps(m, ensure_ascii=False), flush=True)
        if m["t"] == "focus":
            focus[0] = m.get("player")
        elif m["t"] == "cmd":
            ws.send({"t": "ack", "id": m.get("id"), "ok": True})
        elif m["t"] == "cover_req":
            counts["cover_req"] += 1
            if "cover" in features:
                ws.s.sendall(frame(2, struct.pack("!I", m["id"]) + TINY_JPEG))
            else:
                ws.send({"t": "cover_err", "id": m.get("id")})
        elif m["t"] == "lyrics_req":
            counts["lyrics_req"] += 1
            kind = answers[0][min(answers[1], len(answers[0]) - 1)] if answers[0] else "ok"
            answers[1] += 1
            if "lyrics" in features and kind == "ok":
                ws.send({"t": "lyrics", "id": m.get("id"), "found": True, "synced": True, "lines": LYRICS})
            elif "lyrics" in features and kind == "error":
                ws.send({"t": "lyrics", "id": m.get("id"), "found": False, "error": "upstream_failed"})
            else:
                ws.send({"t": "lyrics", "id": m.get("id"), "found": False})

    session[0] = open_session(a.pair)
    if session[0] is None:
        return 1
    print("ready")

    failures = 0
    for i, step in enumerate(steps):
        label = f"step {i + 1}/{len(steps)}"
        if "disconnect" in step:
            session[0].s.close()
            print(f"{label}: disconnected, away for {step['disconnect']} s", flush=True)
            time.sleep(step["disconnect"])
            session[0] = open_session()
            if session[0] is None:
                return 1
            print(f"{label}: connected again", flush=True)
            focus[0] = None  # the device announces its focus again when it has one
            continue
        if "lyrics_answers" in step:
            answers[0], answers[1] = step["lyrics_answers"], 0
        for msg in step.get("send", []):
            session[0].send(msg)
        print(f"{label} sent, waiting {step.get('wait', 1)} s", flush=True)
        session[0].poll(step.get("wait", 1), handle)
        if session[0].closed:
            print("the device closed the connection", file=sys.stderr)
            return 1
        if "expect_focus" in step:
            ok = focus[0] == step["expect_focus"]
            failures += not ok
            print(("check ok:   " if ok else "CHECK FAILED: ") + f"the device shows {focus[0]!r}, expected {step['expect_focus']!r}")
        for name, want in step.get("expect_counts", {}).items():
            ok = counts[name] == want
            failures += not ok
            print(("check ok:   " if ok else "CHECK FAILED: ") + f"{name} was sent {counts[name]} time(s), expected {want}")
    session[0].poll(a.linger, handle)
    session[0].s.close()
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
