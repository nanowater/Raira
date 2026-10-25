#include <cstdio>
#include <lvgl.h>
#include "nvs_flash.h"
#include "music_app.h"
#include "nvs.h"
#include "esp_err.h"

// For now app_main only prints the LVGL version; it does not start the app yet. This flag keeps the call to
// music_app_start() in the program anyway: the linker drops what nobody calls, so without a call a platform function
// that is still missing would stay unnoticed until app_main really starts the app. The flag is volatile so that the
// compiler cannot remove the call; it is never true.
static volatile bool s_start_app = false;

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
        MusicAppDeps deps;
        deps.store = settings_store_create();
        deps.wifi  = wifi_manager_create();
        music_app_start(std::move(deps));
    }
}
