#include <cstdio>
#include <lvgl.h>

extern "C" void app_main(void)
{
    std::printf("LVGL %d, %d, %d, %s\n", LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR, LVGL_VERSION_PATCH, LVGL_VERSION_INFO);
    lv_init();
}
