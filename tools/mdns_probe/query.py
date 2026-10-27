#!/usr/bin/env python3
"""Asks the harness responder DNS questions over multicast (RFC 6762) with an independent client and asserts the answers:
PTR for the service, SRV with the port, TXT with proto/id/name, an A record, the service enumeration, and that a question about
some other service is not answered by *our* instance. Usage: query.py <port>. Exit code 0 = all passed."""
import socket
import struct
import sys
import time

PORT = int(sys.argv[1])
INSTANCE = "Raira (probe)._raira._tcp.local"
FAILED = []


def check(name, ok, detail=""):
    print(("ok    " if ok else "FAIL  ") + name + (f"  ({detail})" if detail and not ok else ""))
    if not ok:
        FAILED.append(name)


def enc(n):
    return b"".join(bytes([len(label)]) + label.encode() for label in n.split(".") if label) + b"\0"


def read_name(d, p):
    out, jumped, end = [], False, p
    while True:
        n = d[p]
        if n == 0:
            p += 1
            break
        if n & 0xC0 == 0xC0:
            if not jumped:
                end = p + 2
            p, jumped = ((n & 0x3F) << 8) | d[p + 1], True
            continue
        out.append(d[p + 1:p + 1 + n].decode("utf-8", "replace"))
        p += 1 + n
    return ".".join(out), (end if jumped else p)


def parse(d):
    _, _, qd, an, ns, ar = struct.unpack("!HHHHHH", d[:12])
    p, recs = 12, []
    for _ in range(qd):
        _, p = read_name(d, p)
        p += 4
    for _ in range(an + ns + ar):
        name, p = read_name(d, p)
        t, _, ttl, rl = struct.unpack("!HHIH", d[p:p + 10])
        p += 10
        rd = d[p:p + rl]
        if t == 12:
            v = read_name(d, p)[0]
        elif t == 33:
            v = {"port": struct.unpack("!H", rd[4:6])[0], "target": read_name(d, p + 6)[0]}
        elif t == 16:
            v, i = [], 0
            while i < len(rd):
                v.append(rd[i + 1:i + 1 + rd[i]].decode())
                i += 1 + rd[i]
        elif t == 1:
            v = socket.inet_ntoa(rd)
        else:
            v = rd
        recs.append((name, t, ttl, v))
        p += rl
    return recs


sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.settimeout(2.0)
sock.bind(("0.0.0.0", 0))


def ask(qname, qtype):
    """All records that answer `qname` within a short window. Other responders on the network (a real hub, KDE Connect...) may
    answer the same question, so this does not stop at the first reply: the checks look for *our* records among them."""
    sock.sendto(struct.pack("!HHHHHH", 0, 0, 1, 0, 0, 0) + enc(qname) + struct.pack("!HH", qtype, 1 | 0x8000), ("224.0.0.251", 5353))
    end = time.time() + 1.3
    got = []
    while time.time() < end:
        sock.settimeout(max(0.05, end - time.time()))
        try:
            data, _ = sock.recvfrom(9000)
        except socket.timeout:
            break
        got += parse(data)
    return got


recs = ask("_raira._tcp.local", 12)
ptr = [r for r in recs if r[1] == 12 and r[0] == "_raira._tcp.local" and r[3] == INSTANCE]
check("PTR names our instance", len(ptr) == 1, recs)
srv = [r[3] for r in recs if r[1] == 33 and r[0] == INSTANCE]
check("SRV carries the port and the host name", srv and srv[0]["port"] == PORT and srv[0]["target"] == "raira-probe.local", srv)
txt = [r[3] for r in recs if r[1] == 16 and r[0] == INSTANCE]
check("TXT has proto, id and name", txt and {"proto=1", "id=probe123", "name=Raira (probe)"} <= set(txt[0]), txt)
check("an A record with an IPv4 address comes along", any(r[1] == 1 and r[0] == "raira-probe.local" for r in recs), recs)
check("every record has a TTL of 120 s", all(r[2] == 120 for r in recs if r[0] in ("_raira._tcp.local", INSTANCE, "raira-probe.local")))

recs = ask("raira-probe.local", 1)
check("an A question for the host name is answered", any(r[1] == 1 and r[0] == "raira-probe.local" for r in recs), recs)
recs = ask(INSTANCE, 33)
check("an SRV question for the instance is answered", any(r[1] == 33 and r[0] == INSTANCE for r in recs), recs)
recs = ask("_services._dns-sd._udp.local", 12)
check("service enumeration lists _raira._tcp", any(r[3] == "_raira._tcp.local" for r in recs if r[1] == 12), recs)
recs = ask("_nothing-like-this._tcp.local", 12)
check("a question about another service is not answered by our instance", not any(INSTANCE in str(r[3]) or r[0] == INSTANCE for r in recs), recs)

print(f"\n{len(FAILED)} failed" if FAILED else "\nall passed")
sys.exit(1 if FAILED else 0)
