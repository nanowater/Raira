#pragma once

#include <lvgl/lvgl.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void music_ui_create(void);

/* Album art widget handle (created in music_ui_create). */
lv_obj_t * music_ui_album_art(void);

/*
 * Set album art from a PNG encoded buffer held in RAM.
 * The buffer is copied internally (caller may free the input after return).
 * The decoded pixels stay cached by LVGL; the encoded copy is kept alive
 * until the next call or music_ui_clear_album_art().
 */
void music_ui_set_album_art_png(const void * data, size_t size);

/* Raw decoded pixels (caller-owned lifetime; data must stay alive). */
void music_ui_set_album_art_raw(const void * data, uint32_t w, uint32_t h, lv_color_format_t cf);

void music_ui_clear_album_art(void);

#ifdef __cplusplus
} /* extern "C" */
#endif
