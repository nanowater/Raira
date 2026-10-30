# Raira

A media controller in the style of the Spotify Car Thing that is not tied to one music service: a small 800×480 touch screen
with a rotary encoder that shows what is playing on your PC (cover, title, progress, synced lyrics) and sends play/pause, skip,
seek, shuffle, repeat and volume back.

The device only controls; the music plays on the PC. A small program on the PC, the **bridge**, reads the players over MPRIS
(TIDAL through `tidal-hifi`, Spotify, browsers and anything else that speaks MPRIS) and talks to the device over WiFi.

## How it fits together

- **The device is the hub.** It announces itself on the network (mDNS, `_raira._tcp`) and runs a WebSocket server.
  Sources such as the bridge find it, pair once with a six-digit code shown on the device, and then push player state and
  answer requests for covers and lyrics. Several PCs can be connected at once.
- **The bridge** (`bridge/`, Rust) runs on the PC, has a settings page at `http://127.0.0.1:18801/` and a tray icon. It also
  looks up synced lyrics on [LRCLIB](https://lrclib.net), so the device needs no HTTPS.
- **The UI** (`src/`, C++23 with [LVGL](https://lvgl.io)) is the same code on the Linux simulator and on the device.
  Everything OS-specific sits behind `src/platform/`.

The protocols are described in `docs/HUB_PROTOCOL.md` and `docs/BRIDGE_PROTOCOL.md`.

## Status

| part | state |
|---|---|
| Linux simulator (`lvglsim`) acting as the hub | works |
| Bridge on Linux (MPRIS, tidal-hifi supplement, lyrics, tray, settings page) | works |
| Bridge on Windows | not started |
| ESP32-P4 firmware (board: JC4880P443C_I_W, P4 + C6) | **runs on the board**: display (MIPI-DSI, rotated to landscape by the PPA), touch, WiFi over the C6, pairing with the bridge, fonts, hardware JPEG; ~60 fps at rest, 28 min soak without a reboot; encoder/buttons, a multi-hour soak and the cost of the track-change transition are open |
| ESP32-S3 firmware (board: JC8048W550) | builds; has not run on a screen (the first unit's display was dead); the same project, selected by chip |

Playlist and queue browsing exists in the UI but no real source provides data yet (`lvglsim -F` shows it with sample data).
`docs/PORTING.md` has the ESP32 status (P4 first), the constraints found on the board, the memory budget and checklists.

## Try it on one PC (Linux)

Needs a C++23 compiler, CMake, SDL2, libturbojpeg, libwebsockets and nlohmann_json, plus a Rust toolchain for the bridge.

    cmake -B build && cmake --build build -j
    build/bin/lvglsim                 # the simulator, which is the hub on port 18322
    cd bridge && cargo run            # the bridge; it finds the hub over mDNS

Then open Settings > Sources > Pair a source on the simulator and type the code on the bridge's settings page
(`http://127.0.0.1:18801/`). If mDNS does not find the hub, start the bridge with `--device 127.0.0.1:18322`.

To install the bridge for your user and start it at every login: `tools/install_bridge.sh` (options are in the
script's header, `--uninstall` removes it).

## Firmware

ESP-IDF v6.1 project in `firmware/`. `tools/firmware_build.sh` builds it and prints the size; flashing and the partition layout
are in `docs/PORTING.md`.

## Checks

`tools/run_checks.sh` runs the bridge's unit tests, formatting and lint, a conformance suite for the simulator's WebSocket
server, an mDNS probe, the device's focus/reconnect/lyrics-retry rules against a fake source and the firmware build.
`tools/README.md` lists them.

## Credits and licenses

This project is MIT licensed (`LICENSE`). It includes [LVGL](https://github.com/lvgl/lvgl) (MIT, in `lvgl/`),
[nlohmann/json](https://github.com/nlohmann/json) (MIT), subsets of Noto Sans CJK (SIL OFL 1.1, `assets/fonts/OFL.txt`) and
a bitmap font generated from DejaVu Sans Mono. Lyrics come from LRCLIB; please keep its rate limits in mind.
