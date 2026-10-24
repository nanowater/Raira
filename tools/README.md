# tools/

Checks that need neither a real player nor a real device nor your own settings. Everything they start (a virtual display, the
simulator, a fake source, test servers) is stopped again by PID, temp files go to `mktemp -d` (honours `$TMPDIR`) and the test
ports (18397-18399) are outside the real ones (hub 18322, bridge 18801). Run them before committing:

    tools/run_checks.sh              # bridge unit tests, WebSocket server, mDNS responder, the device's "Automatic" focus
    tools/run_checks.sh --network    # + the C++ vs Rust lyrics comparison (asks LRCLIB, about a minute)

The device checks (`pick`, `reconnect`, `lyrics_retry`, `lyrics_none`, `lyrics_retry_track_change`) need `build/bin/lvglsim` (`cmake -B build && cmake --build build -j`), Xvfb and ImageMagick's `import`; it is skipped without them.

| tool | what it checks |
|---|---|
| `ws_conformance/run.sh` | The simulator's `WsServer` (libwebsockets) against a hand-written client: RFC 6455 example handshake, text and binary echo incl. 64-bit lengths, ping/pong, fragmentation, close, other paths refused, garbage survived. |
| `mdns_probe/run.sh` | The simulator's mDNS responder: PTR/SRV/TXT/A answers and TTL, service enumeration, no answer for other services. Other responders on the machine (a real hub, KDE Connect) may answer the same questions; the probe collects all replies and looks for its own. The first question comes 0.5 s after the start, which also guards against a responder that is deaf while it announces. |
| `font_image/run.sh` | The format of the firmware's `fonts` flash partition (`src/fonts/font_image.*`): builds a real image with `make_fonts_image.py`, parses it (both fonts byte for byte, 16-byte aligned) and then damaged ones (erased flash, wrong magic/version/count, entries outside the image or overflowing, cut image). |
| `make_fonts_image.py` | Not a check: packs the two Noto subsets into the image for the `fonts` partition (`firmware/build/fonts.bin`); fails when it does not fit the partition in `firmware/partitions.csv`. The command to write it to the board is in the script's header. |
| `firmware_build.sh` | Builds `firmware/` with ESP-IDF to check that the portable core still compiles and links for the ESP32-S3; prints the app size and the number of warnings. `--clean` starts from nothing. Exit code 2 when ESP-IDF is not installed (the check is then skipped). Part of `run_checks.sh`. |
| `install_bridge.sh` | Not a check: installs the bridge for the current user and registers the systemd user service (see the script's header; `--dry-run` shows what it would do). |
| `lyrics_retry/run.sh` | The bridge's lyrics lookup against a fake LRCLIB (`fake_lrclib.py`) whose search breaks once, twice or for good: one lost request must not become "no lyrics" (it used to, and was remembered for a week), a blip is retried once after 2 s, a lasting failure is an error that is not stored. Port 18398, throw-away cache. |
| `lyrics_compare/compare.sh [songs.tsv]` | `src/lyrics.cpp` (device) and `bridge/src/lyrics.rs` (PC) must give identical line lists. Songs: `title<TAB>artists<TAB>album<TAB>seconds`. A song LRCLIB does not answer for is skipped, not failed. |
| `fake_source.py` | A hub source without a real player: authenticates (or pairs with `--pair CODE`), plays a scenario of players and states and prints what the device sends back. A step with `"expect_focus"` makes the exit code 1 when the device shows another player. Scenarios: `playing`, `idle`, `boundary` (paused 150 ms either side of a lyric line: the highlight must flip), `pick` (the source's own pick must beat a newer playing player), `reconnect` (the connection drops and returns: the screen must go blank meanwhile, and the same track must come back without the device asking for its cover and lyrics again, another track must be asked for; run it with `--features cover,lyrics`, so the fake answers with a built-in JPEG and eight synced lines), `lyrics_retry` / `lyrics_none` / `lyrics_retry_track_change` (a lookup that failed is asked again 8 s later, one that found nothing is not, a new track ends the retrying; `--features lyrics`), or `--scenario-file my.json`. A step `{"lyrics_answers": ["error", "ok"]}` sets how the next `lyrics_req` are answered (`ok`, `none`, `error`; the last repeats). A step `{"disconnect": S}` closes the connection and reconnects after S seconds; `"expect_counts"` checks how many `cover_req` / `lyrics_req` the device has sent. The fake answers the device's requests as they arrive. |
| `seed_device.py HOME` | A throw-away `$HOME` for `lvglsim -S` in which the fake source is already paired. |
| `ui_run.sh` | The simulator as the hub on a virtual display plus a fake-source scenario plus screenshots, for looking at a device-side change: `tools/ui_run.sh --scenario boundary --shots 6,11,15 --out DIR`. |

Not covered: how the screen looks (only `ui_run.sh` screenshots, judged by eye), the C++ core has no unit tests, nothing runs on
an ESP32 or on Windows, and long-running behaviour (reconnect loops, memory) is untested.

Adding a scenario: put steps `{"send": [messages], "wait": seconds, "expect_focus": "<player id>"}` in `SCENARIOS` of
`fake_source.py` (helpers `players()` and `state()` build the messages of docs/HUB_PROTOCOL.md) or in a JSON file.
