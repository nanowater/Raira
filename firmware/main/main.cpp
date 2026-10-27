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
#include <thread>
#include <chrono>

// For now app_main only prints the LVGL version; it does not start the app yet. This flag keeps the call to
// music_app_start() in the program anyway: the linker drops what nobody calls, so without a call a platform function
// that is still missing would stay unnoticed until app_main really starts the app. The flag is volatile so that the
// compiler cannot remove the call; it is never true.
static volatile bool s_start_app = false;
static std::unique_ptr<MdnsAdvertiser> g_mdns;
static const char * TAG = "main";

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

    if(s_start_app) {
        auto store   = settings_store_create();
        HubBoot boot = hub_boot_load(*store, "Music Controller");
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
}
