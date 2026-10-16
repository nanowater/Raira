#pragma once

#include <lvgl/lvgl.h>
#include <stdbool.h>
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
    const char * quality; /* short audio-quality text for the badge, "" to hide it */
    int quality_tier;     /* music_ui_quality_t: picks the badge color */
    bool playing;
    bool shuffle;
    int repeat; /* music_ui_repeat_t */
} music_ui_state_t;

/* Declared in the order tidal-hifi cycles them: off -> all -> single -> off. */
typedef enum { MUSIC_UI_QUALITY_NORMAL, MUSIC_UI_QUALITY_LOSSLESS, MUSIC_UI_QUALITY_HIRES } music_ui_quality_t;

typedef enum { MUSIC_UI_REPEAT_OFF, MUSIC_UI_REPEAT_ALL, MUSIC_UI_REPEAT_SINGLE } music_ui_repeat_t;

typedef enum {
    MUSIC_UI_CMD_PREVIOUS,
    MUSIC_UI_CMD_PLAY_PAUSE,
    MUSIC_UI_CMD_NEXT,
    MUSIC_UI_CMD_SHUFFLE,
    MUSIC_UI_CMD_REPEAT
} music_ui_cmd_t;

/* Called on the LVGL thread when a transport button is pressed; must not block. */
typedef void (*music_ui_transport_cb_t)(music_ui_cmd_t cmd);
void music_ui_set_transport_cb(music_ui_transport_cb_t cb);

/* Called on the LVGL thread when the user drags/taps the progress bar and lets go; `seconds` is the
 * chosen position in the track. Must not block. */
typedef void (*music_ui_seek_cb_t)(int seconds);
void music_ui_set_seek_cb(music_ui_seek_cb_t cb);

/* Show/hide the "connection lost" banner (hidden by default; only shown while disconnected).
 * LVGL thread only. */
void music_ui_set_connected(bool connected);

typedef struct {
    const char * text;
    int time_ms; /* ignored when the lyrics are not synced */
} music_ui_lyric_line_t;

/* Show lyrics (strings are copied). Synced lyrics follow the playback position and
 * highlight the current line; unsynced ones are just a scrollable text. LVGL thread only. */
void music_ui_set_lyrics(const music_ui_lyric_line_t * lines, size_t count, bool synced);

/* Replace the lyrics area with a single message (e.g. loading / not found). */
void music_ui_set_lyrics_message(const char * msg);

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
