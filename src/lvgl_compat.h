#pragma once

/* One place that knows how to include LVGL: the simulator build exposes it as <lvgl/lvgl.h>, the
 * ESP-IDF LVGL component as "lvgl.h". */
#if defined(__has_include)
#if __has_include("lvgl.h")
#include "lvgl.h"
#else
#include <lvgl/lvgl.h>
#endif
#else
#include <lvgl/lvgl.h>
#endif
