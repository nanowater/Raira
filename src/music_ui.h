#pragma once

#include <lvgl/lvgl.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Cover widget edge length in px; pass this to the decoder for 1:1 drawing. */
#define MUSIC_UI_COVER_SIZE 250

void music_ui_create(void);

/* Album art widget handle (created in music_ui_create). */
lv_obj_t * music_ui_album_art(void);

typedef struct {
    const char * title;
    const char * artists;
    int current_sec;
    int duration_sec;
} music_ui_state_t;

/* Update now-playing widgets. LVGL thread only (e.g. from lv_async_call). Strings are copied. */
void music_ui_update(const music_ui_state_t * st);

/*
 * Set album art from an encoded image held in RAM. Only formats whose LVGL decoder
 * accepts memory sources work here (PNG via LodePNG); JPEG must be decoded first
 * and passed to music_ui_set_album_art_raw().
 * The buffer is copied internally (caller may free the input after return);
 * the copy is kept alive until the next call or music_ui_clear_album_art().
 * The image is stretched to the widget size.
 */
void music_ui_set_album_art_encoded(const void * data, size_t size);

/* Raw decoded pixels. The buffer is copied internally, like the encoded variant. */
void music_ui_set_album_art_raw(const void * data, uint32_t w, uint32_t h, lv_color_format_t cf);

void music_ui_clear_album_art(void);

#ifdef __cplusplus
} /* extern "C" */
#endif
