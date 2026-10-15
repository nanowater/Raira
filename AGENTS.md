# AGENTS.md — music-controller

## Project purpose

Build a **Spotify Car Thing–style media control remote** (HMI) that is **not locked to any one service** (notably not Spotify). First supported backend is **TIDAL**; Spotify / Apple Music / YouTube Music come later, then long-tail sources (browser players, YouTube videos, other streams).

The device is a *controller*, not a player: it displays now-playing metadata and sends transport commands; audio playback happens on the host PC (audio stack) via the service-specific bridge.

## Hardware (target)

- MCU: ESP32-S3.
- Display: 5" capacitive touchscreen (LVGL UI).
- Input: rotary encoder — volume adjust (turn), play/pause (click) by default.
- Power: **USB-powered for v1** (battery-powered optional later).
- Connectivity to host PC: WiFi HTTP first; serial (USB CDC) and BLE later.

## Software stack

- UI framework: **LVGL 9.2**.
- Dev/simulator: this repo's `lv_port_linux` base (SDL2 window, Kconfig-driven, CMake). Used for prototyping the UI.
- Final target: port UI onto ESP32-S3 (ESP-IDF). Treat the Linux simulator as the *reference UI* during development; keep UI code portable (avoid Linux-only calls in `src/music_ui.*`).
- Service bridge: host PC runs **tidal-hifi** (TIDAL, 1st target) which exposes a local HTTP API on `http://<host>:47836`. Device talks to it over WiFi.

## Repository layout

- `src/main.cpp` — entry point.
- `src/music_ui.h` / `src/music_ui.cpp` — LVGL UI builder (currently a stub: title label + Play button). **Main work happens here.**
- `src/lib/` — LVGL display/input backends (SDL/DRM/FB/Wayland/X11/GLFW).
- `docs/tidal-hifi-swagger.json` — tidal-hifi local API spec (OpenAPI 3.1.0, v8.1.3, 28 paths). **Use this, not the old official TIDAL OAS**, for integration work.
- `docs/JC8048W550/` — vendor reference demos; not part of the build.
- `boards/` — configs for various embedded boards (NXP i.MX93, etc.); ESP32 target config to be added.
- `CMakeLists.txt`, `Kconfig`, `lv_conf.defaults` — build/config.

## Official TIDAL API constraints (do not fight them)

- Official OpenAPI (`tidal-music.github.io/.../tidal-api-oas.json`) has PlaybackState / PlayQueue endpoints, but they require `r_usr`/`w_usr` scopes and `required-access-tier: INTERNAL` (TIDAL first-party only).
- Therefore external apps **cannot** read/control TIDAL "now playing" via the official API. We bridge through the local tidal-hifi API instead. See `docs/tidal-hifi-swagger.json`.

## TIDAL transport contract (v1)

Base URL: `http://<host>:47836` (port fixed by tidal-hifi; configurable in app settings).

Read now-playing:
- `GET /current` → `MediaInfo`:
  - `title`, `artists`/`artist`, `album`, `status` (`playing`/`paused`/...),
  - `current`, `currentInSeconds`, `duration`, `durationInSeconds`,
  - `playingFrom`, `url`, `image`/`icon`, `localAlbumArt`,
  - `favorite`, `player: { status, shuffle, repeat }`,
  - `audioQuality: { quality, badgeText, bitDepth, sampleRate, codec }`.
- `GET /health` for connection status; `GET /current/image`, `GET /current/audio-quality` for details.

Playback control:
- `POST /player/next`, `/player/previous`, `/player/play`, `/player/pause`, `/player/playpause`,
- `POST /player/shuffle/toggle`, `/player/repeat/toggle`,
- `PUT /player/seek/absolute`, `/player/seek/relative`,
- `PUT /player/volume` (0.0–1.0).
- Legacy `GET` variants exist (`/next`, `/play`, etc.) but prefer `POST`.

Polling: tidal-hifi exposes no SSE; poll `/current` (e.g. 1–2s) and diff `title`/`url`/`currentInSeconds`.

## Lyrics (planned, v1.x)

- TIDAL lyrics are INTERNAL-tier → external fetch needed.
- Option: `GET https://lrclib.net/api/get?artist_name=...&track_name=...&album_name=...&duration=...` → `syncedLyrics` (LRC, time-stamped → highlight current line by `currentInSeconds`) / `plainLyrics`.
- Open questions: whether to standardize on LRCLIB for all services or per-service lyrics providers; offline caching.

## Service abstraction (roadmap)

Tier 1: TIDAL (local bridge).
Tier 2: Spotify, Apple Music, YouTube Music (each needs its own host-side bridge / official API).
Tier 3: browser players, YouTube videos, generic streams (likely a browser extension/companion that posts now-playing).

UI/service code should depend on a common normalized `MediaInfo`-like model, not on tidal-hifi JSON directly.

## Encoder UX (default)

- Turn: volume up/down (`PUT /player/volume`).
- Click: play/pause (`POST /player/playpause`).
- Long-press / touch reserved for skip / seek / source switching (TBD).

## Conventions for edits

- LVGL API version is 9.x (`lv_screen_active`, `lv_obj_get_child`, etc.).
- `music_ui_create()` is the single UI entry point called from `main.cpp` (extern "C").
- Keep UI functions pure LVGL; no HTTP/blocking calls on the LVGL thread. Run transport/polling in a separate task/thread and push updates to LVGL via `lv_async_call` (or LVGL timer) — especially important once ported to ESP32.
- HTTP/JSON parsing: prefer a small, embeddable library (e.g. `cpp-httplib` + `nlohmann/json`) so the same code compiles on both Linux sim and ESP32 (check SSL/TLS needs; tidal-hifi local API is plain HTTP).
- Update `docs/tidal-hifi-swagger.json` if tidal-hifi version changes.

## Known constraints / do-not-do

- Do not attempt to use the official TIDAL OpenAPI for now-playing or control (INTERNAL tier).
- Do not block the LVGL main loop with synchronous HTTP.
- Do not ship hardcoded host IP; make it configurable (NVS/flash settings for device, config file/CLI for sim).

## Open / TBD

1. Port strategy from Linux sim (SDL) to ESP-IDF (device) — which LVGL ESP port, display driver (RGB panel / MIPI-DSI), touch driver.
2. Transport priority order after WiFi (serial vs BLE) and framing for serial.
3. Host discovery for tidal-hifi (mDNS vs static config).
4. Battery/charge path and sleep/screen-off timer.
5. Offline caching of album art and lyrics.
6. HiDPI/color depth for the 5" panel.