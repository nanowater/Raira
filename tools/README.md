# tools/

Checks that need neither a real player nor a real device nor your own settings. Everything they start (a virtual display, the
simulator, a fake source, test servers) is stopped again by PID, temp files go to `mktemp -d` (honours `$TMPDIR`) and the test
ports (18397-18399) are outside the real ones (hub 18322, bridge 18801). Run them before committing:

    tools/run_checks.sh              # bridge unit tests, WebSocket server, mDNS responder, the device's "Automatic" focus
    tools/run_checks.sh --network    # + the C++ vs Rust lyrics comparison (asks LRCLIB, about a minute)

The device check needs `build/bin/lvglsim` (`cd build && ninja`), Xvfb and ImageMagick's `import`; it is skipped without them.

| tool | what it checks |
|---|---|
| `ws_conformance/run.sh` | The simulator's `WsServer` (libwebsockets) against a hand-written client: RFC 6455 example handshake, text and binary echo incl. 64-bit lengths, ping/pong, fragmentation, close, other paths refused, garbage survived. |
| `mdns_probe/run.sh` | The simulator's mDNS responder: PTR/SRV/TXT/A answers and TTL, service enumeration, no answer for other services. Other responders on the machine (a real hub, KDE Connect) may answer the same questions; the probe collects all replies and looks for its own. The first question comes 0.5 s after the start, which also guards against a responder that is deaf while it announces. |
| `lyrics_compare/compare.sh [songs.tsv]` | `src/lyrics.cpp` (device) and `bridge/src/lyrics.rs` (PC) must give identical line lists. Songs: `title<TAB>artists<TAB>album<TAB>seconds`. A song LRCLIB does not answer for is skipped, not failed. |
| `fake_source.py` | A hub source without a real player: authenticates (or pairs with `--pair CODE`), plays a scenario of players and states and prints what the device sends back. A step with `"expect_focus"` makes the exit code 1 when the device shows another player. Scenarios: `playing`, `idle`, `boundary` (paused 150 ms either side of a lyric line: the highlight must flip), `pick` (the source's own pick must beat a newer playing player), or `--scenario-file my.json`. |
| `seed_device.py HOME` | A throw-away `$HOME` for `lvglsim -S` in which the fake source is already paired. |
| `ui_run.sh` | The simulator as the hub on a virtual display plus a fake-source scenario plus screenshots, for looking at a device-side change: `tools/ui_run.sh --scenario boundary --shots 6,11,15 --out DIR`. |

Not covered: how the screen looks (only `ui_run.sh` screenshots, judged by eye), the C++ core has no unit tests, nothing runs on
an ESP32 or on Windows, and long-running behaviour (reconnect loops, memory) is untested.

Adding a scenario: put steps `{"send": [messages], "wait": seconds, "expect_focus": "<player id>"}` in `SCENARIOS` of
`fake_source.py` (helpers `players()` and `state()` build the messages of docs/HUB_PROTOCOL.md) or in a JSON file.
