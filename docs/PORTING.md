# Porting notes: simulator → ESP32-S3 (JC8048W550)

Status: the app core is separated from the platform layer and builds/runs on the Linux simulator.
Nothing here has been built or run on ESP-IDF yet (no ESP-IDF toolchain/hardware available when this
was written); the ESP-IDF column below is the intended mapping, to be verified.

## Layout

| Part | Files | Notes |
|---|---|---|
| **App core** (portable) | `src/music_app.*`, `music_ui.*`, `media_poller.*`, `tidal_client.*`, `lyrics.*`, `cover_decoder.*`, `http_util.cpp`, `media_info.h`, `fonts/*`, `lvgl_compat.h` | Depends only on LVGL, nlohmann_json, the C++ standard library (threads, chrono) and the two interfaces below. CMake target `music_core`. |
| **Platform interfaces** | `src/platform/http.h`, `src/platform/jpeg.h` | The only things the core needs from the OS. |
| **Simulator platform** | `src/platform_sim/http_curl.cpp` (libcurl), `jpeg_turbo.cpp` (libjpeg-turbo), `src/main.cpp`, `src/lib/` | CMake targets `music_platform_sim`, `lvglsim`. `main.cpp` also holds sim-only input glue (mouse wheel / arrow keys → volume). |
| **ESP32 platform** (to write) | `HttpClient` + `jpeg_decode_bgrx` + `app_main` | See below. |

Entry point for both: `music_app_start(std::unique_ptr<MediaSource>)` (LVGL initialised, on the LVGL thread);
`main()` / `app_main` builds the service (`std::make_unique<TidalClient>(cfg)`) and passes it in.
Volume input hook: `media_poller_volume_step(int delta)` (call from the encoder driver).

## What the ESP32 side has to provide

(The hub direction in `docs/HUB_PROTOCOL.md` adds one more platform piece: an implementation of `platform/ws_server.h`
on ESP-IDF's `esp_http_server` with WebSocket support (`CONFIG_HTTPD_WS_SUPPORT`), plus the `mdns` component to advertise
`_musicbridge._tcp`. The portable hub core (`hub_source.*`) needs nothing else; with the hub, the device no longer needs
`HttpClient` for the music service, and TLS disappears once lyrics are fetched by the source.)

0. **`WifiManager` and `SettingsStore`** (`platform/wifi.h`, `platform/settings.h`) for the settings page: the
   simulator has a mock WiFi (fake networks, no real radio) and a JSON file. On the device: `esp_wifi` in STA mode
   (scan and connect are non-blocking, the UI polls `scan_running()` / `state()` from a 200 ms LVGL timer) and NVS
   for `AppSettings` (server address, WiFi credentials). Set the touch indev's long-press time to ~800 ms
   (`lv_indev_set_long_press_time`), the settings open on a long press of the cover.

1. **`HttpClient` / `http_client_create()`** (`platform/http.h`): blocking GET/PUT/POST without body, timeouts,
   user agent, and a `cancelled()` callback polled during the transfer. Intended: `esp_http_client`
   (keep-alive via one handle per thread; HTTPS with the ESP certificate bundle for LRCLIB and the TIDAL
   image CDN). Cancellation: `esp_http_client_cancel_request()` / closing from the poll loop, or chunked reads
   that check `cancelled()` between reads.
2. **`jpeg_decode_bgrx()`** (`platform/jpeg.h`): JPEG → BGRX8888. Intended: ESP-IDF `esp_jpeg` (TJpgDec) or
   JPEGDEC. **Open: TIDAL serves progressive JPEGs** (the sim decoder handles them). If the chosen decoder
   cannot, fetch a baseline size/variant from the CDN or convert on the host. Allow the decoder to
   downscale (1/2, 1/4, 1/8) to save time and RAM.
3. **`app_main`**: init NVS, WiFi, display (RGB panel) and touch (GT911) → LVGL port → `music_app_start(cfg)`
   with `TidalConfig` loaded from NVS. Feed the rotary encoder into `media_poller_volume_step()`.
4. **Threads**: the core uses `std::thread`, `std::mutex`, `std::condition_variable`, `std::chrono`.
   ESP-IDF supports these through its pthread layer; set stack sizes/priority with `esp_pthread_set_cfg()`
   before creating `poll_loop` and `lyrics_loop` (JSON parsing + resampling need roughly 16-32 KB; verify).
5. **LVGL config** (`lv_conf.h`) must match what the code uses: TinyTTF (`LV_USE_TINY_TTF`, file support
   off), Montserrat 12/14/16/22/28/30/36/44, `LV_USE_SLIDER`, `LV_USE_IMAGE`, `LV_COLOR_DEPTH 16`,
   `LV_FONT_MONTSERRAT_*` as above, draw buffers sized for 800×480. The code uses newer LVGL 9 setters
   (`lv_obj_set_hidden`, `lv_obj_set_scrollable`, `lv_obj_set_clickable`): pin the ESP LVGL component to the
   same version as the `lvgl/` directory in this repo: it is **10.0.0-dev** (`build/bin/lvglsim -V` prints it), not the
9.2 the older notes mention — the published ESP-IDF LVGL component (9.x) lacks those APIs. Use the same LVGL
sources (e.g. an ESP-IDF component that points at this tree).
6. **Fonts** are embedded with `.incbin` (`src/fonts/noto_sans_kr.c`, two ~3.6 MB OTF files in `assets/fonts/`).
   In ESP-IDF use `EMBED_FILES` or keep that asm; they live in flash (16 MB), mapped read-only. With OTA, two
   app slots need to fit the ~7.3 MB of fonts each — consider a separate read-only font partition.
7. **C++ exceptions**: nlohmann_json can throw. The code uses `parse(..., nullptr, false)` and checks types
   before `get<>`, but ESP-IDF builds with exceptions off by default: either enable
   `CONFIG_COMPILER_CXX_EXCEPTIONS` or build with `JSON_NOEXCEPTION` and re-check the `get<>` calls.
8. **Large buffers** should land in PSRAM: set `CONFIG_SPIRAM_USE_MALLOC` (big `malloc`/`new` go to PSRAM) or
   use `heap_caps_malloc(..., MALLOC_CAP_SPIRAM)` for the cover and background buffers.

## Memory budget (estimates from the code, not measured)

| Item | Size | Where |
|---|---|---|
| Fonts (Regular + Bold subset) | 3.58 + 3.73 MB | flash |
| Cover shown by the UI (400×400 XRGB8888) | 640 KB | PSRAM |
| Ambient background, two images for the cross-fade (800×480 RGB565) | 2 × 768 KB | PSRAM |
| Track change, transient on the poll thread | decoded JPEG (640×640 BGRX, 1.6 MB, freed after decode) + cover 640 KB + background 768 KB (until the UI took them) | PSRAM |
| LVGL draw buffers (800×480, RGB565) | 0.4-0.8 MB | internal/PSRAM |

So a track change peaks at roughly 5 MB of the 8 MB PSRAM. First things to try if that is too much: a
decoder that downscales to 1/2 (640→320 for the small layout), a smaller background (e.g. 400×240 scaled),
a single background image instead of two (no cross-fade), or the 250 px cover only.

## Not portable / sim only
`src/main.cpp` (CLI/env host config, SDL backends), `src/lib/`, `install_volume_input()` (mouse wheel),
`docs/JC8048W550/` (vendor demos, not part of the build).
