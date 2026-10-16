# AGENTS.md — music-controller

## Project purpose

Build a **Spotify Car Thing–style media control remote** (HMI) that is **not locked to any one service** (notably not Spotify). First supported backend is **TIDAL**; Spotify / Apple Music / YouTube Music come later, then long-tail sources (browser players, YouTube videos, other streams).

The device is a *controller*, not a player: it displays now-playing metadata and sends transport commands; audio playback happens on the host PC (audio stack) via the service-specific bridge.

## Hardware (target)

- MCU: ESP32-S3.
- Display: 5" capacitive touchscreen, 800×480 (LVGL UI). Planned board: **JC8048W550** (ESP32-S3-WROOM-1: 16 MB flash, 8 MB PSRAM; vendor docs in `docs/JC8048W550/`). All UI coordinates assume 800×480.
- Input: rotary encoder — volume adjust (turn), play/pause (click) by default.
- Power: **USB-powered for v1** (battery-powered optional later).
- Connectivity to host PC: WiFi HTTP first; serial (USB CDC) and BLE later.

## Software stack

- UI framework: **LVGL 9.2**.
- Dev/simulator: this repo's `lv_port_linux` base (SDL2 window, Kconfig-driven, CMake). Used for prototyping the UI.
- Final target: port UI onto ESP32-S3 (ESP-IDF). Treat the Linux simulator as the *reference UI* during development; keep UI code portable (avoid Linux-only calls in `src/music_ui.*`).
- Service bridge: host PC runs **tidal-hifi** (TIDAL, 1st target) which exposes a local HTTP API on `http://<host>:47836`. Device talks to it over WiFi.

## Repository layout

- `src/main.cpp` — entry point; CLI/env config (`-s host[:port]`, `TIDAL_HIFI_HOST`), starts the UI and the poller.
- `src/music_ui.h` / `src/music_ui.cpp` — LVGL UI (cover, title/artists, progress + times, shuffle/prev/play/next/repeat row, lyrics panel). Pure LVGL, no networking; fed through `music_ui_update`, `music_ui_set_lyrics*`, `music_ui_set_album_art_*`; reports button presses via `music_ui_set_transport_cb`.
- `src/media_info.h` — service-agnostic now-playing model (`MediaInfo`).
- `src/tidal_client.*` — blocking tidal-hifi client (libcurl, one reused handle): `/current`, cover download, transport POSTs. The only place that knows tidal-hifi JSON.
- `src/lyrics.*` — LRCLIB lookup, candidate ranking, LRC parsing.
- `src/cover_decoder.*` — JPEG → 250×250 XRGB8888 (turbojpeg) so the UI only receives pixels.
- `src/media_poller.*` — background threads (poll `/current` every 500 ms + lyrics fetch) and the UI mailbox that hands results to the LVGL thread.
- `src/fonts/` — `font_mono_16.c` (DejaVu Sans Mono, digits and ":", via lv_font_conv) and `noto_sans_kr.c` (embeds the Noto Sans KR subsets with `.incbin`). Font files and regeneration commands: `assets/fonts/README.md`.
- `src/lib/` — LVGL display/input backends (SDL/DRM/FB/Wayland/X11/GLFW).
- `docs/tidal-hifi-swagger.json` — tidal-hifi local API spec (OpenAPI 3.1.0, v8.1.3, 28 paths). **Use this, not the old official TIDAL OAS**, for integration work.
- `docs/JC8048W550/` — vendor reference demos; not part of the build.
- `boards/` — configs for various embedded boards (NXP i.MX93, etc.); ESP32 target config to be added.
- `CMakeLists.txt`, `Kconfig`, `lv_conf.defaults`, `configs/get_started.defconfig` — build/config.

## Build / run (simulator)

- Needs libcurl, libturbojpeg (pkg-config), nlohmann_json, SDL2.
- `cd build && cmake .. && ninja`, then `build/bin/lvglsim [-s host[:port]]` (default `localhost:47836`).
- Changing a `.incbin`-embedded font is tracked via `OBJECT_DEPENDS`; if a font change does not show up, re-run cmake.

## Official TIDAL API constraints (do not fight them)

- Official OpenAPI (`tidal-music.github.io/.../tidal-api-oas.json`) has PlaybackState / PlayQueue endpoints, but they require `r_usr`/`w_usr` scopes and `required-access-tier: INTERNAL` (TIDAL first-party only).
- Therefore external apps **cannot** read/control TIDAL "now playing" via the official API. We bridge through the local tidal-hifi API instead. See `docs/tidal-hifi-swagger.json`.

## TIDAL transport contract (v1)

Base URL: `http://<host>:47836` (port fixed by tidal-hifi; configurable via `-s` / `TIDAL_HIFI_HOST`).

Read now-playing:
- `GET /current` → `MediaInfo`:
  - `title`, `artists` (a **joined string**; `artistsArray` has the list), `album`, `status` (`playing`/`paused`/...),
  - `current`, `currentInSeconds`, `duration`, `durationInSeconds` (whole seconds),
  - `playingFrom`, `url`, `image`/`icon`, `localAlbumArt`,
  - `favorite`, `player: { status, shuffle (bool), repeat ("off"/"all"/"single") }`,
  - `audioQuality: { quality, badgeText, bitDepth, sampleRate, codec }`.
- `GET /health` for connection status; `GET /current/image`, `GET /current/audio-quality` for details.
- Covers: `/current/image` serves a file tidal-hifi rewrites after the metadata changes, so while skipping tracks it lags or returns an in-between track's art (and is labelled `image/png` while being JPEG). We download the `image` URL from `/current` instead (swap `1280x1280` for `320x320`) and use `/current/image` only as a fallback.
- Metadata can be briefly inconsistent after a track change (e.g. new `url` with old `title`); key anything derived from it on all fields, not just `url`.

Playback control:
- `POST /player/next`, `/player/previous`, `/player/play`, `/player/pause`, `/player/playpause`,
- `POST /player/shuffle/toggle`, `/player/repeat/toggle` (repeat cycles off → all → single),
- `PUT /player/seek/absolute`, `/player/seek/relative`,
- `PUT /player/volume` (0.0–1.0; request body format is not documented in the swagger — check before use).
- Legacy `GET` variants exist (`/next`, `/play`, etc.) but prefer `POST`.
- The swagger summary of `/player/shuffle/toggle` ("Play the previous song") looks like a copy-paste error.

Polling: tidal-hifi exposes no SSE; we poll `/current` every 500 ms (≈1 KB, <1 ms locally) over a reused connection. Buttons trigger an immediate extra poll. The UI flips play/pause, shuffle and repeat icons optimistically and ignores polled values for up to 2 s until they agree.

## Lyrics (implemented)

- TIDAL lyrics are INTERNAL-tier → fetched from **LRCLIB** (`https://lrclib.net/api`, no auth, user-agent set, requests only on track change).
- Lookup (`src/lyrics.cpp`): candidates = exact `GET /get?track_name&artist_name&album_name&duration` + free-text `GET /search?q=<title without "(feat…)"> <first artist>` (finds entries filed under another title form, e.g. "あぶく - Bubble"). We score them ourselves: length within 5 s (closer is better), synced lyrics, original script (kana/Hangul/Han ≥ 50 % of the text, so romanized versions lose), album match, exact-lookup bonus; penalty for remix/live/karaoke/… tags the playing title lacks; instrumental entries and synced lyrics whose timestamps run past the track length (a different edit) are skipped.
- HTTP 429 is retried with backoff; lookups are debounced by 500 ms and cancelled as soon as the track changes (`lyrics_fetch(..., cancelled)`).
- Parsing keeps timing-only LRC lines as "gap" entries and appends an estimated end marker after the last line, so the highlight clears in interludes and after the song's last line. Unsynced lyrics are shown as a scrollable text.
- UI: 28 px Bold, current line white (color eases, view glides to it), edges fade out instead of clipping, position interpolated between polls, view follows seeks (to the next line, or the last line once the lyrics are over). The current line deliberately keeps the same font size as the others.
- Open questions: standardize on LRCLIB for all services or per-service providers; offline caching; fetching lyrics on the host instead of the device (TLS cost on ESP32).

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
- Keep UI functions pure LVGL; no HTTP/blocking calls on the LVGL thread. Run transport/polling in a separate task/thread.
- **Never call LVGL (including `lv_async_call`, which creates a timer) from a background thread.** Post closures to the mutex-protected queue in `media_poller.cpp` (`post_to_ui`); a 20 ms LVGL timer runs them on the LVGL thread.
- `lv_obj_clean()` / deleting children fires scroll events while it runs: drop your own references to the children first (see `lyrics_clear_labels`), or the handler will touch deleted objects.
- Use the dedicated LVGL 9 setters (`lv_obj_set_scrollable`, `lv_obj_set_hidden`) — `lv_obj_add_flag`/`remove_flag` are deprecated here.
- LVGL's `anim_duration` for scrolling labels is the *total* travel time; derive it from text width for a constant speed (see `TITLE_SCROLL_PX_PER_S`).
- Fonts: text uses Noto Sans KR via LVGL TinyTTF from data embedded with `.incbin` (Regular for UI, Bold for lyrics; glyphs: Latin, kana, Hangul, ~7k Han from KS X 1001 + JIS X 0208, no simplified-only Chinese). Times use the generated mono bitmap font. Both subsets are the KR face, so Japanese kanji show Korean glyph variants.
- HTTP/JSON parsing: JSON uses `nlohmann/json`; HTTP is libcurl on the sim. For ESP32 the HTTP, JPEG and thread pieces (`tidal_client`, `lyrics`, `cover_decoder`, `media_poller`) need ESP-IDF equivalents behind the same interfaces. tidal-hifi itself is plain HTTP; LRCLIB and the TIDAL image CDN are HTTPS.
- Update `docs/tidal-hifi-swagger.json` if tidal-hifi version changes.

## Known constraints / do-not-do

- Do not attempt to use the official TIDAL OpenAPI for now-playing or control (INTERNAL tier).
- Do not block the LVGL main loop with synchronous HTTP, and do not touch LVGL from other threads.
- Do not ship hardcoded host IP; make it configurable (NVS/flash settings for device; `-s` / `TIDAL_HIFI_HOST` for sim — the sim default `localhost` is fine).
- Do not hammer LRCLIB (it rate-limits with HTTP 429): at most one lookup (≤2 requests) per settled track.

## Open / TBD

1. Port strategy from Linux sim (SDL) to ESP-IDF (device) — which LVGL ESP port, display driver (RGB panel / MIPI-DSI), touch driver. The sim draws a mouse cursor via `lv_indev_set_cursor` in the SDL backend; the touch indev on device has none.
2. ESP32 specifics: HTTPS for LRCLIB/CDN (mbedTLS, certificates, RAM), JPEG decoding (does the ESP decoder handle the progressive JPEGs TIDAL serves? else fetch a baseline size), the two ~3.7 MB font subsets in flash (16 MB flash; weigh against OTA partitions).
3. WiFi / settings page (SSID list, password with on-screen keyboard, tidal-hifi host) with NVS storage.
4. Volume + encoder UX once the hardware arrives (needs the `PUT /player/volume` body format).
5. Transport priority order after WiFi (serial vs BLE) and framing for serial.
6. Host discovery for tidal-hifi (mDNS vs static config).
7. Battery/charge path and sleep/screen-off timer.
8. Offline caching of album art and lyrics.
9. Theme cleanup: only the screen background is customized; widgets without explicit colors still get the default light LVGL theme.
