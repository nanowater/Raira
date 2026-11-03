# Porting notes: simulator → ESP32 (ESP32-P4 on the JC4880P443C_I_W)

Status (2026-10-31): the app core is separated from the platform layer and runs on the Linux simulator **and on the ESP32-P4 board** (see "ESP32-P4
board" at the end for what was verified, the constraints found and what is open). The sections in between were written for the first plan, an
ESP32-S3 board (JC8048W550), which is no longer supported (removed 2026-11-02; it is in the git history): read them as history, the memory
numbers there are estimates from before anything ran.

## Layout

| Part | Files | Notes |
|---|---|---|
| **App core** (portable) | `src/music_app.*`, `music_ui.*`, `settings_ui.*`, `browse_ui.*`, `media_poller.*`, `browse_worker.*`, `hub_source.*`, `cover_decoder.*`, `lyrics.h`, `media_info.h`, `fonts/*`, `lvgl_compat.h` | Depends only on LVGL, nlohmann_json, the C++ standard library (threads, chrono) and the interfaces below. CMake target `music_core`. The device is hub-only (2026-10-25): no HTTP client, no TLS. |
| **Platform interfaces** | `src/platform/jpeg.h`, `ws_server.h`, `mdns.h`, `settings.h`, `wifi.h` | What the app needs from the OS (`jpeg.h` and the fonts at link time, the rest handed over in `MusicAppDeps`). |
| **Simulator platform** | `src/platform_sim/jpeg_turbo.cpp` (libjpeg-turbo), `ws_server_lws.cpp` (libwebsockets), `mdns_responder.cpp`, `settings_file.cpp`, `wifi_mock.cpp`, `src/main.cpp`, `src/lib/` | CMake targets `music_platform_sim`, `lvglsim`. `main.cpp` also holds sim-only input glue (mouse wheel / arrow keys → volume). |
| **ESP32 platform** | `firmware/components/music_platform_esp/` + `firmware/main/` | See "ESP-IDF status" below. |

Entry point for both: `music_app_start(MusicAppDeps)` (LVGL initialised, on the LVGL thread);
`main()` / `app_main` builds the `HubSource` (with the WebSocket server), the mDNS announcement, WiFi and the settings store and passes them in.
Volume input hook: `media_poller_volume_step(int delta)` (call from the encoder driver).

## ESP-IDF status (2026-10-23)

ESP-IDF **v6.1** is installed in `~/projects/esp-idf` (clone + `./install.sh esp32p4`; it works with the system's Python 3.14; the cross
compiler is GCC 15.2). `firmware/` is the ESP-IDF project; the core compiles and links for the ESP32-P4.

| Part | How |
|---|---|
| Build | `tools/firmware_build.sh` (or by hand: `. ~/projects/esp-idf/export.sh`, `cd firmware`, `idf.py set-target esp32p4`, `idf.py build`). It is part of `tools/run_checks.sh` when ESP-IDF is found. |
| LVGL | the repository's own `lvgl/` (10.0.0-dev) as a component: `EXTRA_COMPONENT_DIRS` in `firmware/CMakeLists.txt` points at it, nothing is copied. Its Kconfig supplies the LVGL options. |
| Core | `firmware/components/music_core/` lists the same `src/*.cpp` files the simulator builds (no copies; `platform_sim/`, `main.cpp`, `lib/` are not part of it). |
| nlohmann_json | `firmware/components/nlohmann_json/`: the 3.12.0 single header (same version as the simulator), MIT. |
| C++ standard | **C++23** for the simulator (`CMAKE_CXX_STANDARD`) and the firmware (`-std=gnu++23` per component; ESP-IDF's default is gnu++26). |
| Settings | `firmware/sdkconfig.defaults` (16 MB flash, octal PSRAM, C++ exceptions, `partitions.csv`, LVGL: 16-bit colour, clib malloc, the Montserrat sizes the UI names, TinyTTF, LodePNG). It is only read when `sdkconfig` is created: after changing it, `tools/firmware_build.sh --clean`. |
| Partitions | `firmware/partitions.csv`, as in "Flash layout" below; checked with ESP-IDF's `gen_esp32part.py`. |

**The core links** (since `app_main` has a never-taken call to `music_app_start`, the linker must resolve everything the core needs; the earlier link-less builds proved nothing about that). `firmware/components/music_platform_esp/` is the ESP32 side of `src/platform/`, linked as a whole archive (the core only calls into it):

| File | Implements | How |
|---|---|---|
| `font_partition.cpp` | `noto_sans_kr_get()`, `noto_sans_kr_bold_get()` (`src/fonts/noto_sans_kr.c`, which `.incbin`s 7 MB, is not in the firmware) | `esp_partition_mmap()` of the `fonts` partition; the image format is `src/fonts/font_image.h` (parser tested on the PC: `tools/font_image/`), written by `tools/make_fonts_image.py`. Without a valid image the getters return null and `ui_fonts.cpp` falls back to the built-in Montserrat. |
| `jpeg_esp.cpp` | `jpeg_decode_bgrx()` | the **TJpgDec in the chip's ROM** (no flash, no extra component); baseline JPEG only, 1/2, 1/4, 1/8 downscale. The bridge re-encodes covers as baseline, so every cover the device gets can be decoded; a progressive JPEG would be refused. |
| `settings_nvs.cpp` | `settings_store_create()` (`SettingsStore`) | NVS namespace `music`, one string per `AppSettings` field under the same key names as the simulator's JSON (all within NVS's 15-character key limit; a string is at most 4000 bytes, `hub_sources` is the largest). `save` writes the six keys and commits once; `load` returns false when the namespace does not exist yet (first boot). `app_main` runs `nvs_flash_init()` (erase and retry when the partition is full or from another version). |
| `wifi_esp.cpp` | `wifi_manager_create()` (`WifiManager`) | `esp_wifi` in STA mode with the default netif and event loop; scan and connect are started and return at once, the results come back as events (`WIFI_EVENT_SCAN_DONE`: records converted, hidden networks skipped, strongest first; `IP_EVENT_STA_GOT_IP` = Connected; `WIFI_EVENT_STA_DISCONNECTED`: while connecting retried up to 3 times then Failed, after a working connection reconnected). State and flags are atomics, the scan list and the SSID are under a mutex (the event task writes, the LVGL thread reads). `app_main` connects to the saved network at boot. |
| `mdns_esp.cpp` | `mdns_advertiser_create()` (`MdnsAdvertiser`) | the `mdns` component (a managed dependency since IDF 6: `idf_component.yml`, `espressif/mdns`): `mdns_init`, `mdns_hostname_set`, `mdns_instance_name_set`, `mdns_service_add`; `MdnsService::type` (`_raira._tcp`) is split at the dot, the TXT items are built from `MdnsService::txt`. Needs an IP address, so `app_main` starts it once WiFi is connected. |
| `ws_server_esp.cpp` | `ws_server_create()` (`WsServer`) | `esp_http_server` with `CONFIG_HTTPD_WS_SUPPORT` (off by default). One URI, `/source`, `is_websocket`. **IDF 6 does not call the URI handler for the handshake** (`httpd_uri.c`), so `on_open` runs in `ws_pre_handshake_cb` (`CONFIG_HTTPD_WS_PRE_HANDSHAKE_CB_SUPPORT`; an error from it closes the connection, which is how `on_open` refuses a path) and the handler only sees frames (`httpd_ws_recv_frame` called twice: length, then payload). `WsConnId` is the socket fd. `send_*` work from any thread (`httpd_ws_send_data`, one mutex around it). `on_close` comes from `httpd_config_t::close_fn`, which must `close()` the socket itself; `global_user_ctx` carries `this`, with a no-op free function so the server does not `free()` it. The server task's stack is 8 KB (the handler parses JSON). |

**None of these six has run on a chip.** The font format is tested on the PC; the rest only compiled and linked (the simulator's tests use its own WebSocket and mDNS implementations, so they say nothing about the ESP32 ones: the missing `on_open` call was found by reading `httpd_uri.c`, not by a test). The questions for the board: do scan and connect work against a real access point, which `reason` does a wrong password give and are 3 retries the right number (the `esp_wifi_disconnect()` in `connect()` makes its own disconnect event, which uses up one retry), does `save` followed by `load` give the same six values, and do they survive a power cycle (the WiFi password and the pairing secrets are plain text in NVS; NVS encryption is a later decision), does the 9 MB mmap succeed, and is TJpgDec's pool of 16 KB enough for every cover and how long does a 640 px cover take. (An `esp_http_client` implementation existed until 2026-10-25 and went with the HTTP-based modes; it is in the git history.)

Size of the app with the core linked (still without WiFi, `esp_http_server`, mDNS): **1.40 MB of the 2.5 MB slot** (44 % free); **with the WiFi stack (2026-10-25): 1.80 MB (28 % free)**, the driver and lwIP cost about 390 KB; **hub-only, without `esp_http_client`, TLS and the certificate bundle (2026-10-25): 1.54 MB (38 % free)**; **with the HTTP/WebSocket server, mDNS, the hub boot and its JSON code (2026-10-26): 1.74 MB (30 % free, `0x1be390`)**. The first numbers: of that LVGL 644 KB, the core 165 KB, mbedTLS about 100 KB; flash code 804 KB + read-only data 592 KB; internal SRAM 63 KB of 334 KB. The display driver, touch and the real heap use are still to come, so the OTA slots may have to grow (3.4 MB of the flash are spare).

The WebSocket server (`platform/ws_server.h`), mDNS (`platform/mdns.h`), WiFi and the settings store are not called by the core: `app_main` creates them and hands them over (`MusicAppDeps`, `HubSource`). ESP-IDF 6.0 changed that the `esp_http_server` WebSocket handler is no longer called during the handshake (the migration guide, "ESP HTTP Server"); `ws_server_esp.cpp` learns about a new connection from `ws_pre_handshake_cb` instead. The default C library is now Picolibc (check `nlohmann_json` number parsing and `std::thread` there).

## What the ESP32 side has to provide

(The hub in `docs/HUB_PROTOCOL.md` needs an implementation of `platform/ws_server.h` on ESP-IDF's `esp_http_server` with
WebSocket support (`CONFIG_HTTPD_WS_SUPPORT`), plus the `mdns` component to advertise `_raira._tcp`. The portable hub
core (`hub_source.*`) needs nothing else: covers and lyrics come from the sources, so the device has no HTTP client and no
TLS.)

0. **`WifiManager` and `SettingsStore`** (`platform/wifi.h`, `platform/settings.h`) for the settings page: the
   simulator has a mock WiFi (fake networks, no real radio) and a JSON file. On the device: `esp_wifi` in STA mode
   (scan and connect are non-blocking, the UI polls `scan_running()` / `state()` from a 200 ms LVGL timer) and NVS
   for `AppSettings` (player choice, hub identity, paired sources, WiFi credentials). Set the touch indev's long-press time to ~800 ms
   (`lv_indev_set_long_press_time`), the settings open on a long press of the cover.

1. **`jpeg_decode_bgrx()`** (`platform/jpeg.h`): JPEG → BGRX8888. Done with the ROM's TJpgDec (baseline only; the
   bridge re-encodes every cover as baseline JPEG). It downscales (1/2, 1/4, 1/8) to save time and RAM.
2. **`app_main`**: init NVS, WiFi, display (RGB panel) and touch (GT911) → LVGL port → `HubSource` + mDNS →
   `music_app_start(deps)` with the settings loaded from NVS (connect to the saved WiFi network). Feed the rotary encoder into `media_poller_volume_step()`.
   **Done so far** (behind the never-taken `s_start_app` guard, until display and touch exist): settings and device identity (`hub_boot_load`, shared with the simulator), WiFi created and connected to the saved network, `HubSource` created and handed over, and a thread that waits for `WifiState::Connected` and then starts the hub's server (port 18322) and the mDNS announcement. **Still to do:** display, touch, the `lv_timer_handler` loop, the encoder.
3. **Threads**: the core uses `std::thread`, `std::mutex`, `std::condition_variable`, `std::chrono`.
   ESP-IDF supports these through its pthread layer; set stack sizes/priority with `esp_pthread_set_cfg()`
   before creating `poll_loop` and `lyrics_loop` (JSON parsing + resampling need roughly 16-32 KB; verify).
4. **LVGL config** (`lv_conf.h`) must match what the code uses: TinyTTF (`LV_USE_TINY_TTF`, file support
   off), Montserrat 12/14/16/22/28/30/36/44, `LV_USE_SLIDER`, `LV_USE_IMAGE`, `LV_COLOR_DEPTH 16`,
   `LV_FONT_MONTSERRAT_*` as above, draw buffers sized for 800×480. The code uses newer LVGL 9 setters
   (`lv_obj_set_hidden`, `lv_obj_set_scrollable`, `lv_obj_set_clickable`): pin the ESP LVGL component to the
   same version as the `lvgl/` directory in this repo: it is **10.0.0-dev** (`build/bin/lvglsim -V` prints it), not the
9.2 the older notes mention — the published ESP-IDF LVGL component (9.x) lacks those APIs. Use the same LVGL
sources (e.g. an ESP-IDF component that points at this tree).
5. **Fonts** are embedded with `.incbin` (`src/fonts/noto_sans_kr.c`, two ~4.5 MB OTF files in `assets/fonts/`).
   In ESP-IDF use `EMBED_FILES` or keep that asm; they live in flash (16 MB), mapped read-only. With OTA, two
   app slots would each need the ~7.1 MB of fonts: **decided: a separate read-only `fonts` data partition**, see "Flash layout"
   below (`src/fonts/noto_sans_kr.c` then maps the partition instead of using `.incbin`).
6. **C++ exceptions**: nlohmann_json can throw. The code uses `parse(..., nullptr, false)` and checks types
   before `get<>`, but ESP-IDF builds with exceptions off by default: either enable
   `CONFIG_COMPILER_CXX_EXCEPTIONS` or build with `JSON_NOEXCEPTION` and re-check the `get<>` calls.
7. **Large buffers** should land in PSRAM: set `CONFIG_SPIRAM_USE_MALLOC` (big `malloc`/`new` go to PSRAM) or
   use `heap_caps_malloc(..., MALLOC_CAP_SPIRAM)` for the cover and background buffers.

## Memory budget (estimates from the code, not measured)

| Item | Size | Where |
|---|---|---|
| Fonts (Regular + Bold subset) | 4.40 + 4.56 MB | flash (the `fonts` partition, see "Flash layout") |
| Cover shown by the UI (400×400 XRGB8888) | 640 KB | PSRAM |
| Ambient background, two images for the cross-fade (800×480 RGB565) | 2 × 768 KB | PSRAM |
| Track change, transient on the poll thread | decoded JPEG (640×640 BGRX, 1.6 MB, freed after decode) + cover 640 KB + background 768 KB (until the UI took them) | PSRAM |
| Last cover as downloaded, kept to show it again after a reconnect or when switching back to the track | the encoded JPEG, tens of KB to ~100 KB | PSRAM |
| LVGL draw buffers (800×480, RGB565) | 0.4-0.8 MB | internal/PSRAM |

So a track change peaks at roughly 5 MB of the 8 MB PSRAM. First things to try if that is too much: a
decoder that downscales to 1/2 (640→320 for the small layout), a smaller background (e.g. 400×240 scaled),
a single background image instead of two (no cross-fade), or the 250 px cover only.

The poller keeps the last cover *encoded*, not decoded (`art_cache_img` in `media_poller.cpp`): keeping the decoded cover and
background would have cost another 1.4 MB for good. A restore after a reconnect decodes it again (on the poll thread, like a new
track); measure that time on the device (JPEG decode of a 400 px cover on the ESP32-S3) before relying on it being instant.

## Not portable / sim only
`src/main.cpp` (CLI/env host config, SDL backends), `src/lib/`, `install_volume_input()` (mouse wheel),
the vendor's JC8048W550 material (data sheets, pin table, demo sketches), which is not in this repository.

## Flash layout (plan, 16 MB, to be confirmed when the first firmware is built)

Sizes are estimates: the app size is unknown until ESP-IDF builds it (LVGL, WiFi, `esp_http_server`, mdns), so `ota_*` may need
to grow. The fonts are the two `assets/fonts/NotoSansKR-*-subset.otf` files (4.40 + 4.56 MB).

| Partition | Type | Size | Notes |
|---|---|---|---|
| bootloader, partition table | | ~64 KB | |
| `nvs` | data | 24 KB | settings, WiFi credentials, hub secrets (encrypt the secrets) |
| `otadata`, `phy_init` | data | 8 KB + 4 KB | |
| `ota_0`, `ota_1` | app | 2.5 MB each (estimate) | code only, so an update downloads a few MB, not the fonts |
| `fonts` | data | 10 MB, read-only (the image is 9.0 MB: Regular 4.4 + Bold 4.6, Latin, kana, Hangul, ~10.8k Han incl. simplified Chinese GB2312 and Big5 level 1) | both OTF files back to back; the code mmaps it (`esp_partition_mmap`) and hands the pointers to TinyTTF; written only by a full flash, not by OTA |
| spare | | ~3 MB | room for the apps to grow |

First install = one merged image (`esptool merge_bin`: bootloader + partition table + app + fonts) written by the web flasher
(ESP Web Tools: a `manifest.json` that lists the image, served over HTTPS; Chrome/Edge WebSerial). Later updates: OTA from the
bridge's settings page (a possible extension), which only replaces the app slot.

## First boot checklist (when the board arrives)

Do these in order; each one needs the one before it. The first three need no display (watch the serial monitor: `idf.py -p <port> flash monitor`, with the `s_start_app` guard in `firmware/main/main.cpp` temporarily set to true and `music_app_start` left out until display and touch exist; the hub and WiFi part works without them).

1. **Boots at all.** Prints `LVGL ...`, no reset loop. If it resets before that: NVS (`nvs_flash_init`), then `wifi_manager_create()` (its constructor `abort()`s on any init error, which prints the failing call).
2. **WiFi.** Flash once with no saved network and set one through the settings store (or hard-code it for the test). Expect `wifi: event IP_EVENT 0` (GOT_IP) within a few seconds. A wrong password shows `disconnected, reason 15` (4-way handshake timeout) or `2`; check that 3 retries end in `Failed`.
3. **Server and mDNS.** `hub_ptr->start(18322)` must not log "cannot listen". From the PC: `dns-sd -B _raira._tcp` or `avahi-browse -rt _raira._tcp` shows the device with the TXT `proto`, `id`, `name`; `cd bridge && cargo run` lists it under "Found nearby". If the bridge connects: does `on_open` run once (add a log), does pairing complete (the code is shown on the display, so before the display works read it from the log), does `on_close` run when the bridge is stopped, and can the bridge reconnect ten times in a row (a socket leak would show as refused connections).
4. **Memory.** `esp_get_free_heap_size()` / `heap_caps_get_free_size(MALLOC_CAP_INTERNAL)` after boot, after the first connection and after a cover has arrived. Internal RAM is the scarce one: the `httpd` task (8 KB stack), the pthread stacks (the watcher thread uses the default `CONFIG_PTHREAD_TASK_STACK_SIZE_DEFAULT`; raise it if it overflows), the cover frame is read into one `std::string` of up to `max_message` (256 KB) from the heap.
5. **`std::random_device`** (the device id in `hub_boot_load`): two boots with erased NVS must give two different ids. Picolibc is the default C library, so this is not certain.
6. **Fonts partition** (`tools/make_fonts_image.py`, written to the `fonts` partition): `font_partition` logs success and the UI later shows Noto, not Montserrat.
7. **Then** display (RGB panel), touch (GT911), the `lv_timer_handler` loop, the encoder; and finally remove the `s_start_app` guard.

## ESP32-P4 + ESP32-C6 (JC4880P443C_I_W): the plan before the first boot (2026-10-29, history)

_The table below is what was known before the board ran anything: the display, the JPEG decoder and the touch were not written yet. The current
state is in "ESP32-P4 board: state, constraints, open points" at the end of this file._

The first JC8048W550 had a dead display, so the **ESP32-P4 board JC4880P443C_I_W** (P4 + C6 for WiFi, ST7701 480×800 MIPI-DSI, 32 MB PSRAM,
16 MB flash, GT911 touch, CSI camera connector, ES8311 codec, TF card, RS485) is tried next; the S3 support was removed later.
The ordered unit was expected to be a v1.3 (engineering sample, from a review), but `esptool chip-id` reads **ESP32-P4 revision v3.2** (400 MHz, dual core + LP core),
so the vendor's examples (`docs/JC4880P443C_I_W/1-Demo/idf_examples/`, v3.x only) apply and `sdkconfig.defaults.esp32p4` uses IDF's default revision range. (ESP-IDF builds rev <3.0
and rev >=3.0 separately, `CONFIG_ESP32P4_SELECTS_REV_LESS_V3`; a v1.x chip would need that option.) The board enumerates as USB-Serial/JTAG (`303a:1001`, `/dev/ttyACM0`);
the first plug-ins failed with `error -71` (cable/port), a later one worked.

| Part | State |
|---|---|
| Build | `tools/firmware_build.sh esp32p4` (build dir `firmware/build_p4`, `sdkconfig.p4`, `dependencies.lock.esp32p4`). **Compiles and links with IDF 6.1, 0 warnings, 1.77 MB**; nothing has run. |
| Settings | `sdkconfig.defaults` (shared) + `sdkconfig.defaults.esp32p4` (flash, PSRAM, chip revision). |
| WiFi | the P4 has no radio: `espressif/esp_wifi_remote` + `esp_hosted 2.11.*` (SDIO to the C6) make `esp_wifi` available, so `wifi_esp.cpp`, mDNS and the WebSocket server **compile unchanged**; whether they run over it is untested. The C6 needs a matching ESP-Hosted slave firmware (the vendor ships `JC-C6-slave_v2.3.2.bin` in `8-Burn operation/Burn files/`). |
| JPEG | the P4 has **no TJpgDec in ROM**: `jpeg_p4.cpp` is a stub (returns false, no cover shown). Planned: the hardware codec (`esp_driver_jpeg`). |
| Display | not written: MIPI-DSI + ST7701 (vendor config: 2 lanes, 750 Mbit/s, 28 MHz pixel clock, 480×800, porches h 42/12/42, v 8/2/166), shown rotated to 800×480 (software rotation in the vendor's `lvgl_sw_rotation`, or the PPA). |
| Pins (from the schematic, unchecked on the board) | I2C (touch, codec) SDA 7 / SCL 8; touch INT 21, RST 22 (GT911, 0x5D or 0x14); LCD backlight PWM 23; LCD reset 5; C6 SDIO CLK 18, CMD 19, D0 14 (D1..D3 15..17); SD card 39..44; codec I2S 9..13, 48; BOOT button = GPIO35 (do not use for inputs). Header JP1 (2x13, 2.54 mm; pin 1 = the end with 3V3/5V, odd pins on the 3V3 side): 1/3 3V3, 2/4 5V, 5/6 GND, 7 GPIO52, 8 GPIO33, 9 GPIO51, 10 GPIO31, 11 GPIO50, 12 GPIO30, 13 GPIO49, 15 GPIO35, 17 GPIO34, 19 GPIO32, 21 GPIO28, 23/25 the I2C bus (SDA/SCL, shared with touch and codec), 20..26 even = the C6 (UART, IO9, CHIP_PU); avoid 34/35 (strapping) and pins 14..18 (the schematic is ambiguous there). Checked on the board: GPIO33, 31 and 30 are the ones at pins 8, 10 and 12. The board's MX1.25 4-pin sockets are not usable for an encoder: UART0 (GPIO37/38) is the log console and strapping, the UART socket's GPIO27 is driven by the RS485 chip, RS485 has no GPIO, the I2C socket is the touch/codec bus. |
| Not decided | encoder + one extra button: GPIO 28..31 are the proposal; the camera (OV02C10 on the CSI connector) comes later. |

**P4 board verified with the vendor example (2026-10-30):** the factory demo works (display, touch, WiFi). The vendor's `lvgl_sw_rotation` example
(LVGL 9.5, `esp_lvgl_port`, DSI panel, GT911, LVGL software rotation 90°) builds unchanged with IDF 6.1 and runs: boot log `chip revision v3.2`, 32 MB PSRAM at 200 MHz, `Display initialized`,
GT911 id 911; landscape 800×480 correct, touch correct, the LVGL widgets demo shows 60-63 fps when still and ~20 fps while moving. Warnings to act on: the flash is a
Boya chip (`CONFIG_SPI_FLASH_SUPPORT_BOYA_CHIP`), `ledc: GPIO 23 is not usable` (backlight pin, harmless so far). The vendor BSP passes touch INT/RST as NC (the GT911 answers at its default address).
Pieces the vendor BSP uses for the panel: DSI PHY power from LDO channel 3 at 2500 mV, `esp_lcd_new_dsi_bus` (2 lanes, 750 Mbit/s), `esp_lcd_new_panel_io_dbi`,
the vendor's own copy of `esp_lcd_st7701` (it holds the init sequence that makes this panel light up), DPI 28 MHz 480×800, backlight on LEDC (GPIO 23, 5 kHz, 10 bit).

**P4, own firmware, stage 1 (2026-10-30/11-01):** `board_p4.cpp` brings up LDO 3 (2.5 V), the DSI bus (2 lanes, 750 Mbit/s), the DBI command path, the ST7701 panel (vendored driver, default init sequence, reset GPIO 5, DPI 28 MHz 480×800) and the backlight (LEDC, GPIO 23, 5 kHz, 10 bit).
Verified on the board: `esp_lcd_dpi_panel_set_pattern(MIPI_DSI_PATTERN_BAR_VERTICAL)` shows the rainbow bars, so the panel init, DSI lanes, DPI timing and backlight are right. (`LCD ID: FF FF FF` in the log is harmless: the panel cannot be read back.)
Build gotchas: ESP-IDF builds with `-Werror=missing-field-initializers`, so designated initializers must name every field (`.flags = {}`, `.phy_clk_src = {}`); a struct with a deprecated field (`ledc_channel_config_t::intr_type`) is zeroed with `= {}` and assigned field by field instead.

## ESP32-P4 board: state, constraints, open points (2026-10-31)

**Verified on the board** (chip v3.2, 32 MB PSRAM, 16 MB flash): the display (MIPI-DSI, rotated to landscape by the PPA with a non-blocking transfer), the
GT911 touch, WiFi through the C6 (ESP-Hosted over SDIO; the log warns `Host [2.11.0] > Co-proc [2.3.0]`: the C6 runs an older ESP-Hosted firmware
than the host driver, no failure traced to it so far), mDNS + the hub server + pairing with the PC bridge, the fonts partition, covers and
thumbnails through the hardware JPEG codec (~52 ms), about 60 fps at rest, a 28 min run without a reboot or watchdog.

**What matters (found the hard way):**
- *The internal RAM is the scarce resource.* ESP-Hosted allocates its receive buffers from `MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA` and asserts
  (`sdio_push_data_to_queue ... pkt_rxbuff`, a reboot) when that is gone. What keeps it available: LVGL strips of 20 lines (2 × 32 KB, not 2 × 64 KB),
  `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=8` (only blocks under 8 bytes stay internal), smaller task stacks (main 8 KB, app threads 6 KB: measured
  headroom of 1.7 KB and 3 KB, +22 KB), and not re-fetching a cover that cannot be decoded
  (`media_poller.cpp`: with no decoder the poller used to download the same cover twice a second).
  Measured (2026-11-02): free internal RAM 258 KB (largest block 216 KB) and flat over a 2 h soak with a playlist changing tracks. With 64 it kept
  falling (103 → 73 KB in 2.5 h): a heap trace showed the TinyTTF caches (`lv_cache_lru_rb`: glyph info, bitmap and kerning nodes of 12-32 bytes per
  new character, per font instance) filling the internal RAM.
- `CONFIG_FREERTOS_HZ=1000` (at 100 Hz a 5 ms delay is zero ticks), LVGL refresh period 16 ms (the default 33 ms caps the frame rate at 30), the task
  watchdog panics instead of only warning (a stuck LVGL loop was seen once, 415 s in, not reproduced), the app's std::threads are pinned to core 1
  (`esp_pthread`), core 0 keeps the LVGL loop.
- The vendor's ST7701 driver copy is needed (`components/esp_lcd_st7701`); the vendor's macros are C-only and fail under `-Werror=missing-field-initializers`:
  fill structs with `= {}` and assign, or name every field.
- After a crash a soft reset can leave the chip in a state where even the ROM faults (black screen, `Guru Meditation` right after `load:...`): unplug and
  replug USB. Opening the serial port from a PC tool resets the board (DTR/RTS).
- The bootloader has to fit in 24 KB (0x2000..0x8000): frame pointers or more logging push it over.

**Start-up** (measured 2026-11-02, from reset): the C6 answers at ~3 s, WiFi connects at ~7 s, the IP comes at ~8.1 s, the hub listens and announces itself at ~8.6 s. The bridge used to notice that the connection it held was dead only at its next push (up to 30 s), so it was connected at ~16.7 s; with a WebSocket ping every 3 s (dead after 10 s without any frame; the device's httpd answers pings itself) and an immediate retry when the device announces itself over mDNS again, it is connected at ~11.2 s and the first cover shows at ~12.1 s. The start-up screen (logo and dots, `music_ui_boot_show()`) covers that time and ends at the first successful poll.

**Cover quality**: the bridge sends the device a 400 px JPEG (Lanczos3, quality 92); for tidal-hifi from the 640 px picture TIDAL serves (MPRIS's artUrl is a 150 px file). Sizes 24-165 KB; hardware decode 51-63 ms and the background ~270-380 ms are as before.

**CPU while playing** (core 0, LVGL; measured per second): ~9 % at rest, ~4 % idle. The lyrics glide to the next line (450 ms, every few seconds) is
render-bound at ~30 fps: ~175 Kpx per frame (the lyrics panel, not the screen), ~16 ms per frame, ~500 ms of the second, about a third of it the
full-screen background image copied from the PSRAM under the panel, the rest the text. Updating the glide every 2nd or 3rd frame changes nothing (it
is already render-bound); the edge fade of the lyric lines and the colour fade cost nothing measurable; the title scroll ~2 points. Mean `main` while
lyrics play: ~10 %. Not worth more work unless the background copy gets cheaper.

**Track change** (about 1 s of LVGL at up to 93 % of core 0): the new background fades in over the old one as two full-screen images, a full-screen alpha
blend per frame in software. Not reduced yet; two draw units would probably not help (one big blend per strip). The background computation itself
(`cover_background`, ~360 ms) runs on core 1 and does not compete with it. The cover and background resolution is not to be lowered.

**Done:** the EC11 encoder (`firmware/main/encoder.cpp`: PCNT decodes A = GPIO31 / B = GPIO30, KEY = GPIO33 polled; module powered from 3V3, never 5V: the GPIOs are 3.3 V only; one detent = 4 counts = one volume step, `REVERSE` flips the direction), a brightness slider.

**Open:** an extra button (GPIO28 at JP1 pin 21 is free), screen-off, the C6 ESP-Hosted firmware version,
the track-change transition.
