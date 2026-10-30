#include <cstdio>
#include <lvgl.h>
#include "hub_source.h"
#include "nvs_flash.h"
#include "music_app.h"
#include "platform/mdns.h"
#include "platform/wifi.h"
#include "platform/ws_server.h"
#include "nvs.h"
#include "esp_err.h"
#include "esp_log.h"
#include "hub_boot.h"
#include "board.h"
#include <thread>
#include <chrono>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// For now app_main only prints the LVGL version; it does not start the app yet. This flag keeps the call to
// music_app_start() in the program anyway: the linker drops what nobody calls, so without a call a platform function
// that is still missing would stay unnoticed until app_main really starts the app. The flag is volatile so that the
// compiler cannot remove the call; it is never true.
static volatile bool s_start_app = false;

// TEMP(board test): corner labels, a button and a counter, to see the orientation and the touch on the display.
static void board_test_screen()
{
    lv_obj_t * scr = lv_screen_active();
    const struct
    {
        const char * text;
        lv_align_t align;
    } corners[] = {
        {"TOP-LEFT", LV_ALIGN_TOP_LEFT},
        {"TOP-RIGHT", LV_ALIGN_TOP_RIGHT},
        {"BOTTOM-LEFT", LV_ALIGN_BOTTOM_LEFT},
        {"BOTTOM-RIGHT", LV_ALIGN_BOTTOM_RIGHT},
    };
    for(const auto & c : corners) {
        lv_obj_t * l = lv_label_create(scr);
        lv_label_set_text(l, c.text);
        lv_obj_align(l, c.align, 0, 0);
    }
    static int count   = 0;
    lv_obj_t * counter = lv_label_create(scr);
    lv_label_set_text(counter, "taps: 0");
    lv_obj_align(counter, LV_ALIGN_CENTER, 0, -60);

    lv_obj_t * btn = lv_button_create(scr);
    lv_obj_set_size(btn, 240, 90);
    lv_obj_align(btn, LV_ALIGN_CENTER, 0, 20);
    lv_obj_t * bl = lv_label_create(btn);
    lv_label_set_text(bl, "Touch me");
    lv_obj_center(bl);
    lv_obj_add_event_cb(
        btn,
        [](lv_event_t * e) {
            lv_obj_t * c = static_cast<lv_obj_t *>(lv_event_get_user_data(e));
            lv_label_set_text_fmt(c, "taps: %d", ++count);
        },
        LV_EVENT_CLICKED, counter);
}
static std::unique_ptr<MdnsAdvertiser> g_mdns;
static const char * TAG = "main";

extern "C" void app_main(void)
{
    std::printf("LVGL %d.%d.%d %s\n", LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR, LVGL_VERSION_PATCH, LVGL_VERSION_INFO);
    lv_init();
    const bool board_ok = board_init();
    esp_err_t nvs_err   = nvs_flash_init();
    if(nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_err);

    if(s_start_app) {
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
        music_app_start(std::move(deps));
        std::thread([hub_ptr, wifi_ptr, id = boot.settings.hub_device_id, name = boot.settings.hub_device_name]() {
            while(wifi_ptr->state() != WifiState::Connected) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
            if(!hub_ptr->start(18322)) {
                ESP_LOGE(TAG, "hub cannot listen on port 18322, not announcing it");
                return;
            }
            // Temporary, until the display shows the pairing code: open the pairing window (2 min) and print the code.
            ESP_LOGW(TAG, "pairing code: %s", hub_ptr->start_pairing().c_str());
            g_mdns = mdns_advertiser_create();
            MdnsService adv;
            adv.instance = name;
            adv.txt      = {{"proto", "1"}, {"id", id}, {"name", name}};
            g_mdns->start(adv);
        }).detach();
    }

    // TEMP(board test): drive LVGL from this task until music_app_start() runs the UI.
    if(board_ok) {
        board_test_screen();
        while(true) {
            lv_timer_handler();
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }
}
