#include <cstdio>
#include <lvgl.h>

#include "music_app.h"

// For now app_main only prints the LVGL version; it does not start the app yet. This flag keeps the call to music_app_start() in the
// program anyway: the linker drops what nobody calls, so without a call a platform function that is still missing would stay unnoticed
// until app_main really starts the app. The flag is volatile so that the compiler cannot remove the call; it is never true.
static volatile bool s_start_app = false;

extern "C" void app_main(void)
{
    std::printf("LVGL %d.%d.%d %s\n", LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR, LVGL_VERSION_PATCH, LVGL_VERSION_INFO);
    lv_init();
    if(s_start_app) music_app_start(MusicAppDeps{});
}
