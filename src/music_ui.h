#pragma once

#include "browse_ui.h"
#include "lvgl_compat.h"
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Edge length the cover is decoded to. The widget shows it at 250 px next to the lyrics and at 400 px
 * in the lyrics-free layout (LVGL scales it). */
#define MUSIC_UI_COVER_SIZE 400

void music_ui_create(void);

/* Album art widget handle (created in music_ui_create). */
lv_obj_t * music_ui_album_art(void);

typedef struct
{
    const char * title;
    const char * artists;
    int current_sec;
    int duration_sec;
    const char * quality; /* short audio-quality text for the badge, "" to hide it */
    int quality_tier;     /* music_ui_quality_t: picks the badge color */
    bool playing;
    bool shuffle;
    int repeat;      /* music_ui_repeat_t */
    int position_ms; /* -1: only current_sec (whole seconds) is known; else the position in ms, as of this update */
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

/* The volume slider (a speaker button next to the transport controls opens a popup that closes itself after a few
 * seconds): `percent` 0..100 while the slider moves. Not called for changes from outside. */
typedef void (*music_ui_volume_cb_t)(int percent);
void music_ui_set_volume_cb(music_ui_volume_cb_t cb);

/* The volume the service reports (0.0..1.0); ignored while the user is on the slider or just let go. LVGL thread. */
void music_ui_set_volume(double level);

/* Whether the speaker button may show: 0 = automatic (hidden once a rotary encoder is in use), 1 = always, 2 = never.
 * `encoder_present`: the encoder driver saw input. The button also needs MUSIC_UI_CAP_VOLUME. */
void music_ui_set_volume_button(int mode, bool encoder_present);

/* Which transport controls to show: services that cannot drive them are reflected here (unsupported
 * ones are hidden). Bit values mirror MediaCapability in media_source.h (asserted in media_poller.cpp). */
#define MUSIC_UI_CAP_SEEK (1u << 0)
#define MUSIC_UI_CAP_VOLUME (1u << 1)
#define MUSIC_UI_CAP_SHUFFLE (1u << 2)
#define MUSIC_UI_CAP_REPEAT (1u << 3)
#define MUSIC_UI_CAP_PREV_NEXT (1u << 4)
#define MUSIC_UI_CAP_BROWSE (1u << 5) /* the playlist/queue button */

/* Show only the transport controls the service supports (bitmask as above; bits not set hide their
 * control). Call once, after music_ui_create(); LVGL thread only. */
void music_ui_set_capabilities(unsigned caps);

/* Playlist/queue browsing: a button at the bottom right (shown while MUSIC_UI_CAP_BROWSE is set) opens a
 * panel in place of the lyrics; this callback receives what the panel asks for (see browse_ui.h for the lists to
 * feed back). LVGL thread only. */
void music_ui_set_browse_cb(browse_cb_t cb);

/* Blank what the display shows of the track: title, artists, cover, ambient background, lyrics, progress and times.
 * Called when the connection to the music service is lost: the data is long out of date, and the poller keeps what it
 * needs to show it again if the same track comes back. The controls and the banner are not touched. LVGL thread only.
 */
void music_ui_clear_content(void);

/* Show/hide the "connection lost" banner (hidden by default; only shown while disconnected).
 * LVGL thread only. */
void music_ui_set_connected(bool connected);

/* The start-up screen (logo and pulsing dots, on the top layer above everything): show it as early as possible, it
 * hides the half-built UI and the "connection lost" banner while the device starts and connects. music_ui_boot_done()
 * fades it out (the first successful poll calls it; so does a 25 s timer, after that the banner shows the truth). LVGL
 * thread only; both may be called more than once. */
void music_ui_boot_show(void);
void music_ui_boot_done(void);

typedef struct
{
    const char * text;
    int time_ms; /* ignored when the lyrics are not synced */
} music_ui_lyric_line_t;

/* No lyrics for this track: switch to the "now playing" layout (big cover, info on the right). */
void music_ui_set_no_lyrics(void);

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

/* Full-screen background from the cover: opaque RGB565 pixels (w x h = the screen). Copied
 * internally; cross-fades from the previous one. LVGL thread only. */
void music_ui_set_background(const void * rgb565, uint32_t w, uint32_t h);
#define MUSIC_UI_BG_BASE_COLOR 0x12151B /* screen color without a cover */

#ifdef __cplusplus
} /* extern "C" */
#endif
