#include <chrono>
#include <cstdio>
#include <memory>
#include <cstring>
#include <string>
#include <thread>
#include <vector>
#include <lvgl.h>
#include "board.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_pthread.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hub_boot.h"
#include "hub_source.h"
#include "music_app.h"
#include "music_ui.h"
#include "nvs_flash.h"
#include "platform/mdns.h"
#include "platform/settings.h"
#include "platform/wifi.h"
#include "platform/ws_server.h"

static const char * TAG = "main";

#if configGENERATE_RUN_TIME_STATS && configUSE_TRACE_FACILITY
// TEMP(diag): CPU use per task over the last interval, in percent of ONE core's time. IDLE0 / IDLE1 are the idle tasks
// of the cores: the closer to 100, the more that core is waiting. Taken from the FreeRTOS run-time counters
// (microseconds).
static void log_cpu(int64_t interval_us)
{
    static std::vector<TaskStatus_t> prev;
    std::vector<TaskStatus_t> now(uxTaskGetNumberOfTasks() + 4);
    uint32_t total        = 0;
    const UBaseType_t got = uxTaskGetSystemState(now.data(), now.size(), &total);
    now.resize(got);
    if(!prev.empty() && interval_us > 0) {
        std::string line;
        for(const auto & t : now) {
            uint32_t before = 0;
            for(const auto & q : prev) {
                if(q.xHandle == t.xHandle) {
                    before = q.ulRunTimeCounter;
                    break;
                }
            }
            const int pct = static_cast<int>(100.0 * (t.ulRunTimeCounter - before) / interval_us);
            if(pct < 2) continue;
            char buf[48];
            std::snprintf(buf, sizeof buf, "%s=%d%%%s ", t.pcTaskName, pct,
                          t.xCoreID == 0   ? "(c0)"
                          : t.xCoreID == 1 ? "(c1)"
                                           : "");
            line += buf;
        }
        ESP_LOGI(TAG, "cpu (of one core): %s", line.c_str());
    }
    prev = std::move(now);
}

// TEMP(diag): per task the least stack that was ever left (bytes, ESP-IDF counts the high-water mark in bytes). A
// task whose number stays large can get a smaller stack, or one in PSRAM, which frees internal RAM.
static void log_stacks()
{
    std::vector<TaskStatus_t> now(uxTaskGetNumberOfTasks() + 4);
    uint32_t total        = 0;
    const UBaseType_t got = uxTaskGetSystemState(now.data(), now.size(), &total);
    now.resize(got);
    std::string line;
    for(const auto & t : now) {
        char buf[48];
        std::snprintf(buf, sizeof buf, "%s=%u ", t.pcTaskName, (unsigned)t.usStackHighWaterMark);
        line += buf;
    }
    ESP_LOGI(TAG, "stack left (min, bytes): %s", line.c_str());
}
#endif

// TEMP(diag): the internal RAM left at a point of the start-up (the ESP-Hosted receive buffers need it)
static void log_heap(const char * where)
{
    ESP_LOGI(TAG, "heap @ %s: internal free %u (largest %u)", where,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
}
static std::unique_ptr<MdnsAdvertiser> g_mdns;

// The port sources connect to (the hub's WebSocket server), the same as the simulator's default.
static const uint16_t HUB_PORT = 18322;

// What the simulator's main() does, in the same order: settings -> hub (the saved identity and the paired sources) ->
// the app (which builds the UI and starts the poller). The hub listens and announces itself on mDNS once WiFi has an
// address: the user may enter the WiFi password in the settings page, so that can be much later than the start.
static void start_app()
{
    log_heap("start_app");
    auto store   = settings_store_create();
    HubBoot boot = hub_boot_load(*store, "Raira");
    auto wifi    = wifi_manager_create();
    if(!boot.settings.wifi_ssid.empty()) {
        wifi->connect(boot.settings.wifi_ssid, boot.settings.wifi_password);
    }
    WifiManager * wifi_ptr = wifi.get();

    MusicAppDeps deps;
    auto hub = std::make_unique<HubSource>(
        ws_server_create(), HubIdentity{boot.settings.hub_device_id, boot.settings.hub_device_name}, boot.paired);
    HubSource * hub_ptr = hub.get();
    deps.hub            = hub_ptr;
    deps.store          = std::move(store);
    deps.wifi           = std::move(wifi);
    deps.source         = std::move(hub);
    deps.settings       = boot.settings;
    deps.set_brightness = [](int percent) { board_set_backlight(percent); };
    // The background threads of the app (poller, lyrics, browse, the hub start-up) run on core 1; core 0 keeps the LVGL
    // loop. Every std::thread that this task creates from here on takes this configuration. (The cover background alone
    // takes ~0.5 s of CPU per track.)
    esp_pthread_cfg_t pcfg = esp_pthread_get_default_config();
    pcfg.pin_to_core       = 1;
    pcfg.stack_size        = 6144; // the app threads used ~3 KB of the default 8 KB at most
    ESP_ERROR_CHECK(esp_pthread_set_cfg(&pcfg));
    log_heap("before music_app_start");
    music_app_start(std::move(deps));
    log_heap("after music_app_start");

    std::thread([hub_ptr, wifi_ptr, id = boot.settings.hub_device_id, name = boot.settings.hub_device_name]() {
        while(wifi_ptr->state() != WifiState::Connected) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        if(!hub_ptr->start(HUB_PORT)) {
            ESP_LOGE(TAG, "hub cannot listen on port %d, not announcing it", HUB_PORT);
            return;
        }
        log_heap("hub listening");
        g_mdns = mdns_advertiser_create();
        MdnsService adv;
        adv.instance = name;
        adv.port     = HUB_PORT;
        adv.txt      = {{"proto", "1"}, {"id", id}, {"name", name}};
        if(!g_mdns->start(adv)) ESP_LOGW(TAG, "mDNS: the hub is not announced");
        log_heap("mDNS announced");
    }).detach();
}

extern "C" void app_main(void)
{
    std::printf("LVGL %d.%d.%d %s\n", LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR, LVGL_VERSION_PATCH, LVGL_VERSION_INFO);
    lv_init();
    esp_err_t nvs_err = nvs_flash_init();
    if(nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_err);

    // Panel, backlight, touch and the LVGL display/input; false = this board has no display code (yet): nothing to
    // show, no app.
    if(!board_init()) {
        ESP_LOGE(TAG, "no display, the app is not started");
        return;
    }
    // The start-up screen first: start_app() takes seconds (the C6 and WiFi come up), and the half-built UI and the
    // "connection lost" banner must not show meanwhile. One handler call draws the still picture; the dots move
    // once the loop below runs.
    music_ui_boot_show();
    lv_timer_handler();
    start_app();

    // This task owns LVGL from here on: lv_timer_handler() runs the timers, redraws and reads the touch, and says how
    // many ms until it needs to run again. Everything slow lives on other threads (they hand work over through the UI
    // mailbox).
    int64_t next_heap_log = 0;
    while(true) {
        // TEMP(diag): the internal RAM is what the WiFi link to the C6 (ESP-Hosted) needs; watch it while testing.
        if(esp_timer_get_time() >= next_heap_log) {
            ESP_LOGI(TAG, "heap: internal free %u (largest block %u), PSRAM free %u",
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
            next_heap_log = esp_timer_get_time() + 10 * 1000000;
        }
#if configGENERATE_RUN_TIME_STATS && configUSE_TRACE_FACILITY
        // TEMP(diag): CPU per task every second (a track change is a burst of well under 10 s)
        static int64_t last_cpu = 0;
        const int64_t now_us    = esp_timer_get_time();
        if(now_us - last_cpu >= 1000000) {
            log_cpu(last_cpu ? now_us - last_cpu : 0);
            static int tick = 0;
            if(++tick % 10 == 0) log_stacks();
            last_cpu = now_us;
        }
#endif
        uint32_t ms = lv_timer_handler();
        if(ms == LV_NO_TIMER_READY) ms = LV_DEF_REFR_PERIOD;
        vTaskDelay(pdMS_TO_TICKS(ms));
    }
}
