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

- UI framework: **LVGL**, the `lvgl/` directory in this repo (vendored; `.gitmodules` lists it but git tracks it as a plain tree), currently **10.0.0-dev** (`build/bin/lvglsim -V`). The code uses its newer setters (`lv_obj_set_hidden`, `lv_obj_set_clickable`, `lv_obj_set_scrollable`); do not assume 9.2 APIs.
- Dev/simulator: this repo's `lv_port_linux` base (SDL2 window, Kconfig-driven, CMake). Used for prototyping the UI.
- Final target: port UI onto ESP32-S3 (ESP-IDF). Treat the Linux simulator as the *reference UI* during development; keep UI code portable (avoid Linux-only calls in `src/music_ui.*`).
- Service bridge: host PC runs **tidal-hifi** (TIDAL, 1st target) which exposes a local HTTP API on `http://<host>:47836`. Device talks to it over WiFi.

## Repository layout

- `src/main.cpp` — entry point; server address priority: `-s host[:port]` / `TIDAL_HIFI_HOST` > saved setting > default `localhost:47836`; starts the UI and the poller.
- `src/music_ui.h` / `src/music_ui.cpp` — LVGL UI (cover, title/artists, seekable progress bar + times + quality, shuffle/prev/play/next/repeat row, lyrics panel, full-screen cover-derived ambient background, "connection lost" banner). Two layouts, switched by `apply_layout()`: LYRICS (250 px cover + info on the left, lyrics on the right) and NOW_PLAYING (400 px cover on the left, bigger title/artists/progress/controls on the right, no lyrics) — NOW_PLAYING is shown automatically when no lyrics are found (`music_ui_set_no_lyrics`) and LYRICS as soon as lyrics arrive; tapping the cover toggles the lyrics manually (the choice sticks across tracks; tracks without lyrics always get NOW_PLAYING). Switching animates (`request_layout`): the cover glides/scales, the rest fades out, is rearranged and fades back in (~0.4 s). LVGL gestures (swipes) did not fire reliably from simulated mouse drags, hence the tap. Pure LVGL, no networking; fed through `music_ui_update`, `music_ui_set_lyrics*`, `music_ui_set_album_art_*`; reports button presses via `music_ui_set_transport_cb`, progress-bar seeks via `music_ui_set_seek_cb`, and shows only the transport controls the service reports (`music_ui_set_capabilities`; the flags are in `media_source.h`).
- `src/browse_ui.*` — playlist/queue browser panel (tabs Queue | Playlists, playlist → track drill-down with a back arrow; rows are addressed by index, strings are copied, rebuilds run from a 1 ms timer so a tapped row is never deleted inside its own event). In the LYRICS layout it takes the lyrics panel's place: a round list button at the bottom right of `music_ui` opens/closes it (opening forces the LYRICS layout; tapping the cover closes it; `lyrics_available`/`no_lyrics` keep the LYRICS layout while it is open). The button shows while `MEDIA_CAP_BROWSE` is set (`music_ui_set_capabilities`). `src/browse_worker.*` — the browse thread: the panel's requests (open/play/thumbnail) go to a `BrowseSource` (`media_source.h`, separate from `MediaSource`: listing is slow and must not delay the now-playing poll), results come back through `media_post_to_ui`; after playing an item it re-reads the queue 700 ms later. `MusicAppDeps::browse` hands the source over; `media_browse_mode_t` (`media_poller_start`) decides the capability bit: NONE clears it, WHEN_REPORTED keeps what the source reports (the bridge's `browse_*` capabilities), ALWAYS forces it. `BridgeBrowse` (`bridge_client.*`) is the bridge implementation (`-P`); `src/fake_browse.*` — simulator-only canned `BrowseSource` (`lvglsim -F`, forces the button on, replaces the bridge's), no effect on the real player. Data model follows `docs/BRIDGE_PROTOCOL.md` (browse section). Rows show cover thumbnails: items carry an opaque `cover` key, the panel asks the provider (`browse_ui_set_thumb_cb`) only for rows that are on screen or one row away, and the answer (`browse_ui_set_thumb`, 48×48 RGB565, ~4.5 KB each) fills the row later; a placeholder icon shows until then; the cache keeps at most 48 thumbnails besides the ones on screen. **Roadmap, not available now:** the bridge serves no `/browse/*`, so with the bridge the browse button never shows (only `-F` fake data exercises the UI). The Spotify Web API route is blocked because Spotify's development-mode rules (Feb 2026) require the *app owner* to have Premium (plus 5 users max, `/playlists/{id}/items` instead of `/tracks`, playlist contents only for owned/collaborative playlists), and the account in use has none. The code (PKCE login, token store, playlists/tracks/queue/`play_item`, queue→(context,index) matching) is parked on the git branch `spotify-browse`, untested past the login step. Without Premium, playlists simply do not work; a no-API alternative (a bridge-side recent-plays list replayed through MPRIS `OpenUri`) was considered and not built.
- `src/settings_ui.*` — settings overlay (WiFi: scan/pick/password with on-screen keyboard; music-service address; after saving the address it stays on the server page and shows "checking..." → connected/not-connected, reported by the poller's connection callback). Opened by tapping the "연결 끊김" banner or long-pressing (0.8 s) the cover. Pure LVGL; reaches the outside through `SettingsServices`. Persistence and WiFi are platform interfaces (`platform/settings.h`, `platform/wifi.h`); the simulator uses a JSON file (`~/.config/music-controller/settings.json`, mode 0600) and a mock WiFi with fake networks (never touches the PC's WiFi; passwords of 8+ characters "work"). `src/ui_fonts.*` — cached Noto TinyTTF fonts shared by the UIs. Dark LVGL theme with the Noto font as default (`music_ui_create`).
- `src/media_info.h` — service-agnostic now-playing model (`MediaInfo`).
- `src/media_source.h` — `MediaSource`, the interface every music service implements (`get_current`, `get_cover`, transport commands, `capabilities()` — the UI hides the controls it does not report —, `set_endpoint`/`endpoint()` to switch the host at runtime); the poller only talks to this. Used from the poller thread only, except the one-shot `capabilities()` in `media_poller_start()`.
- `src/bridge_client.*` — `BridgeSource`, the `MediaSource` for the host bridge (`lvglsim -P [-s host[:port]]`, default `localhost:47900`; the saved server address is ignored in this mode). Commands are JSON POSTs (`HttpRequest::body`/`content_type`), toggles are computed from the last polled state. Every poll carries the selected player's capabilities (`MediaInfo::caps`, -1 = not reported); the poller applies changes to `g_caps` and the UI (`music_ui_set_capabilities`), so controls appear/disappear when the bridge's selected player changes. A bridge with no player answers 404 `no_player`: treated as idle, not as a lost connection. Sends the access token (`HttpRequest::bearer_token`; `-T`/`MUSIC_BRIDGE_TOKEN` > saved `bridge_token` setting; changeable at runtime through `MediaSource::set_token` → `media_poller_set_token` / `browse_worker_set_token`; the settings page shows an "Access token" row only when `uses_token()`). A wrong token is a 401, which the device shows as "Connection lost"/"Not connected". Cover keys are opaque and include the track id, so every track refetches its cover. Seek/position are whole seconds on the device side (the bridge sends milliseconds).
- `bridge/` — the host bridge, a separate Rust crate (`cd bridge && cargo run -- --bind 0.0.0.0 --port 47900`): axum HTTP server + zbus MPRIS backend (`mpris.rs`), player registry/selection/mapping (`hub.rs`), cover re-encoding to baseline JPEG (`cover.rs`, `image` + `reqwest`), wire types (`model.rs`). TIDAL supplement (`tidal.rs`): for the MPRIS player whose identity is "tidal-hifi" (not a phone mirrored through KDE Connect) the bridge also calls tidal-hifi's HTTP API (`--tidal-hifi host[:port]`, default `127.0.0.1:47836`, `--no-tidal-hifi` to disable): `/current` fills `shuffle`, `repeat` and `quality` (+ the matching capabilities) when MPRIS has none; MPRIS always wins; the quality is dropped when tidal-hifi's title differs from MPRIS's (lag after a skip); an unreachable tidal-hifi only removes those capabilities. Commands: shuffle toggles only when the state differs; repeat walks the off→all→single cycle, waiting for each step to show up in `/current` (tidal-hifi reports a change ~1 s late and swallows a toggle sent before that, so a two-step change takes ~2 s and blocks that command's poller turn). Access: a 16-character shared token (`auth.rs`) is required on every request unless the bridge is bound to loopback (`--no-auth` / `--token X` override; stored in `~/.config/music-bridge/token`, printed at start; plain HTTP, so it is not protection against sniffing). Linux only so far (GSMTC for Windows is the next backend); no GUI yet (tray icon + web settings page planned). Player ids derive from `DesktopEntry`/`Identity`, never the bus name (tidal-hifi's is `chromium.instance<PID>`). KDE Connect phones show up as players (`service: "other"`). See `docs/BRIDGE_PROTOCOL.md`.
- `src/tidal_client.*` — `TidalClient`, the `MediaSource` for tidal-hifi (through `HttpClient`, one reused connection): `/current`, cover download (CDN URL with a size picked from `want_px`, `/current/image` as fallback), transport requests. The only place that knows tidal-hifi JSON/endpoints.
- `src/lyrics.*` — LRCLIB lookup, candidate ranking, LRC parsing (HTTP through `HttpClient`).
- `src/platform/` — interfaces the core needs from the OS: `http.h` (`HttpClient`; POST/PUT can carry a small body, `ok()` is any 2xx), `jpeg.h` (`jpeg_decode_bgrx`). `src/platform_sim/` — their simulator implementations (libcurl, libjpeg-turbo). `src/music_app.*` — portable entry `music_app_start(MusicAppDeps)` (source, wifi, settings store, current settings, address validator); `src/lvgl_compat.h` — the one place that includes LVGL. See `docs/PORTING.md` for the ESP32 plan, memory budget and open items.
- `src/cover_decoder.*` — JPEG → 400×400 XRGB8888 (via `jpeg_decode_bgrx`; the widget scales it to 250 or 400) and the ambient background (`cover_background`: blurred color grid stretched to 800×480, dithered RGB565) so the UI only receives pixels.
- `src/media_poller.*` — background threads (poll `/current` every 500 ms + lyrics fetch), the UI mailbox that hands results to the LVGL thread, the volume logic (`media_poller_volume_step`), and the connection state (`media_conn_state_t`: CONNECTED/LOST/UNKNOWN after an endpoint switch), reported through `music_ui_set_connected` and the settings-page callback (`media_poller_set_connection_cb`).
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
- Covers: `/current/image` serves a file tidal-hifi rewrites after the metadata changes, so while skipping tracks it lags or returns an in-between track's art (and is labelled `image/png` while being JPEG). We download the `image` URL from `/current` instead (swap `1280x1280` for `640x640`) and use `/current/image` only as a fallback.
- Metadata can be briefly inconsistent after a track change (e.g. new `url` with old `title`); key anything derived from it on all fields, not just `url`.

Playback control:
- `POST /player/next`, `/player/previous`, `/player/play`, `/player/pause`, `/player/playpause`,
- `POST /player/shuffle/toggle`, `/player/repeat/toggle` (repeat cycles off → all → single),
- `PUT /player/seek/absolute?seconds=N`, `/player/seek/relative?seconds=±N` (query parameter, no body; absolute seek is wired to the progress bar),
- `PUT /player/volume?volume=0.5` (0.0–1.0, query parameter, no body).
- Legacy `GET` variants exist (`/next`, `/play`, etc.) but prefer `POST`.
- The swagger summary of `/player/shuffle/toggle` ("Play the previous song") looks like a copy-paste error.

Polling: tidal-hifi exposes no SSE; we poll `/current` every 500 ms (≈1 KB, <1 ms locally) over a reused connection. Buttons trigger an immediate extra poll. The UI flips play/pause, shuffle and repeat icons optimistically and ignores polled values for up to 2 s until they agree.

## Lyrics (implemented)

- TIDAL lyrics are INTERNAL-tier → fetched from **LRCLIB** (`https://lrclib.net/api`, no auth, user-agent set, requests only on track change).
- Lookup (`src/lyrics.cpp`): candidates = exact `GET /get?track_name&artist_name&album_name&duration` + free-text `GET /search?q=<title without "(feat…)"> <first artist>` (finds entries filed under another title form, e.g. "あぶく - Bubble"). We score them ourselves: length within 5 s (closer is better), synced lyrics, original script (kana/Hangul/Han ≥ 50 % of the text, so romanized versions lose), album match, exact-lookup bonus; penalty for remix/live/karaoke/… tags the playing title lacks; instrumental entries and synced lyrics whose timestamps run past the track length (a different edit) are skipped.
- HTTP 429 is retried with backoff; lookups are debounced by 500 ms and cancelled as soon as the track changes (`lyrics_fetch(..., cancelled)`).
- Parsing keeps timing-only LRC lines as "gap" entries and appends an estimated end marker after the last line, so the highlight clears in interludes and after the song's last line; the UI shows each gap as one blank line (consecutive gaps share it). Unsynced lyrics are shown as a scrollable text.
- UI: 28 px Bold, current line white (color eases, view glides to it), edges fade out instead of clipping, position interpolated between polls, view follows seeks (to the next line, or the last line once the lyrics are over). The current line deliberately keeps the same font size as the others.
- Open questions: standardize on LRCLIB for all services or per-service providers; offline caching; fetching lyrics on the host instead of the device (TLS cost on ESP32).

## Service abstraction (roadmap)

Tier 1: TIDAL (local bridge).
Tier 2: Spotify, Apple Music, YouTube Music (each needs its own host-side bridge / official API).
Tier 3: browser players, YouTube videos, generic streams (likely a browser extension/companion that posts now-playing).

UI/service code should depend on a common normalized `MediaInfo`-like model, not on tidal-hifi JSON directly.

**Adding a service** = one class implementing `MediaSource` (fill `MediaInfo`, map the commands, report `capabilities()`), created in `main()` / `app_main` and passed to `music_app_start()`. Nothing in the UI or poller changes. Per-service flash/RAM cost is small (tens of KB of code; the big buffers — cover, background, JPEG decode — are shared), so prefer one host-side bridge that normalizes several services into one protocol over per-service OAuth/TLS code on the ESP32. Capabilities reach the UI: unsupported controls are hidden (`music_ui_set_capabilities`, called by the poller at startup; without MEDIA_CAP_VOLUME the encoder/volume path is inert).

## Encoder UX (default)

- Turn: volume up/down. Implemented as `media_poller_volume_step(delta)` (2 % per step, thread-safe, coalesced to one `PUT /player/volume?volume=` per 80 ms, optimistic target that falls back to the polled `volume` after 2 s). **No volume UI on purpose** (the TIDAL mobile app has none either). The simulator feeds it from the mouse wheel (up = louder) and Up/Down arrow keys via `install_volume_input()` in `main.cpp`; the device's encoder driver should call `media_poller_volume_step()` directly.
- Click: play/pause (`POST /player/playpause`).
- Long-press / touch reserved for skip / seek / source switching (TBD).

## Conventions for edits

- LVGL API is the 9.x/10.0-dev style (`lv_screen_active`, `lv_obj_get_child`, `lv_obj_set_hidden`, etc.).
- `music_ui_create()` is the single UI entry point (extern "C"); it is called by `music_app_start()` (which also builds the settings page and starts the poller) — `main.cpp` only calls `music_app_start()`.
- Keep UI functions pure LVGL; no HTTP/blocking calls on the LVGL thread. Run transport/polling in a separate task/thread.
- **Never call LVGL (including `lv_async_call`, which creates a timer) from a background thread.** Post closures to the mutex-protected queue in `media_poller.cpp` (`post_to_ui`); a 20 ms LVGL timer runs them on the LVGL thread.
- `lv_obj_clean()` / deleting children fires scroll events while it runs: drop your own references to the children first (see `lyrics_clear_labels`), or the handler will touch deleted objects.
- Use the dedicated LVGL 9 setters (`lv_obj_set_scrollable`, `lv_obj_set_hidden`) — `lv_obj_add_flag`/`remove_flag` are deprecated here.
- LVGL's `anim_duration` for scrolling labels is the *total* travel time; derive it from text width for a constant speed (see `TITLE_SCROLL_PX_PER_S`).
- Fonts: text uses Noto Sans KR via LVGL TinyTTF from data embedded with `.incbin` (Regular for UI, Bold for lyrics; glyphs: Latin, kana, Hangul, ~7k Han from KS X 1001 + JIS X 0208, no simplified-only Chinese). Times use the generated mono bitmap font. Both subsets are the KR face, so Japanese kanji show Korean glyph variants.
- HTTP/JSON parsing: JSON uses `nlohmann/json`; HTTP and JPEG go through `src/platform/` (libcurl / libjpeg-turbo on the sim, ESP-IDF equivalents later). Keep `curl`/`turbojpeg` includes out of the core files (`music_core` target); new OS dependencies get an interface in `src/platform/` first. tidal-hifi itself is plain HTTP; LRCLIB and the TIDAL image CDN are HTTPS.
- Update `docs/tidal-hifi-swagger.json` if tidal-hifi version changes.

## Known constraints / do-not-do

- Do not attempt to use the official TIDAL OpenAPI for now-playing or control (INTERNAL tier).
- Do not block the LVGL main loop with synchronous HTTP, and do not touch LVGL from other threads.
- Do not ship hardcoded host IP; make it configurable (NVS/flash settings for device; `-s` / `TIDAL_HIFI_HOST` for sim — the sim default `localhost` is fine).
- Do not hammer LRCLIB (it rate-limits with HTTP 429): at most one lookup (≤2 requests) per settled track.

## Open / TBD

1. Port strategy from Linux sim (SDL) to ESP-IDF (device) — which LVGL ESP port, display driver (RGB panel / MIPI-DSI), touch driver. The sim draws a mouse cursor via `lv_indev_set_cursor` in the SDL backend; the touch indev on device has none.
2. ESP32 specifics: HTTPS for LRCLIB/CDN (mbedTLS, certificates, RAM), JPEG decoding (does the ESP decoder handle the progressive JPEGs TIDAL serves? else fetch a baseline size), the two ~3.7 MB font subsets in flash (16 MB flash; weigh against OTA partitions).
3. Settings page is built (sim: mock WiFi + JSON file). Device: real `WifiManager` (esp_wifi STA, non-blocking scan/connect) and an NVS-backed `SettingsStore`; store the WiFi password safely (NVS encryption); the first-boot flow when no WiFi is configured; hostname/mDNS instead of typing an IP.
4. Encoder driver on the device (step feed, direction/debounce, acceleration, long-press = mute?) — the volume logic itself is done.
5. Transport priority order after WiFi (serial vs BLE) and framing for serial.
6. Host discovery for tidal-hifi (mDNS vs static config).
7. Battery/charge path and sleep/screen-off timer.
8. Offline caching of album art and lyrics.
9. Theme: the dark default theme with the Noto font is set in `music_ui_create()`; still double-check that stock widgets of other overlays (settings list, keyboard) and future widgets match the hand-styled main UI.
