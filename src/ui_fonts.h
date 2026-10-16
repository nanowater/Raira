#pragma once

#include "lvgl_compat.h"

/* Noto Sans KR (embedded subset) at `px` via TinyTTF, cached per size/weight. `fallback` is used for glyphs
 * outside the subset and when loading fails. Call from the LVGL thread. */
const lv_font_t * ui_noto_font(int32_t px, const lv_font_t * fallback, bool bold = false);
