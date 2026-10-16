#pragma once

#include "lvgl_compat.h"
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Playlist / play-queue browser: a panel that takes the place of the lyrics panel. Pure LVGL, no
 * networking; the app feeds it lists (strings are copied) and gets told what the user picked. All calls
 * on the LVGL thread. Rows are addressed by index into the list last given, so no string ownership is
 * shared with the provider. */

typedef struct {
    const char * title;
    const char * subtitle; /* artists, track count, ... ; may be NULL */
    bool playable;         /* tapping starts playback (a queue row without a known context is view-only) */
    bool current;          /* the row that is playing now: highlighted */
    const char * cover;    /* opaque cover key (the bridge's /cover/<key>); NULL = no thumbnail */
} browse_item_t;

/* Thumbnails are requested lazily, only for rows that are (nearly) on screen, and are small:
 * BROWSE_THUMB_PX square, RGB565 (48 x 48 x 2 = 4.5 KB each, at most a few dozen cached). The provider
 * answers whenever it has the pixels, possibly later and from any time after the request, with
 * browse_ui_set_thumb(). Rows show a placeholder until then. */
#define BROWSE_THUMB_PX 48
typedef void (*browse_thumb_req_cb_t)(const char * cover_key); /* LVGL thread; must not block */
void browse_ui_set_thumb_cb(browse_thumb_req_cb_t cb);
/* Pixels are copied; w and h should be BROWSE_THUMB_PX (they are stretched otherwise). */
void browse_ui_set_thumb(const char * cover_key, const void * rgb565, uint32_t w, uint32_t h);

typedef enum {
    BROWSE_EVT_SHOWN,         /* the panel was opened: refresh the queue and the playlist list */
    BROWSE_EVT_OPEN_PLAYLIST, /* index = row in the playlist list; answer with browse_ui_set_tracks() */
    BROWSE_EVT_PLAY_TRACK,    /* index = row in the open playlist's track list */
    BROWSE_EVT_PLAY_QUEUE     /* index = row in the queue list */
} browse_evt_t;

/* Called on the LVGL thread; must not block. The provider may answer with browse_ui_set_* from inside
 * the callback (the lists are rebuilt a moment later, not while the tapped row is still handling the event). */
typedef void (*browse_cb_t)(browse_evt_t evt, int index);

/* Create the panel as a child of `parent` (hidden). The caller positions/sizes it and shows/hides it. */
lv_obj_t * browse_ui_create(lv_obj_t * parent, int32_t w, int32_t h);
void browse_ui_set_cb(browse_cb_t cb);
bool browse_ui_has_cb(void); /* is anything listening (i.e. is there a browse provider)? */

/* The caller just showed the panel: back to the queue tab and ask the provider to refresh (BROWSE_EVT_SHOWN). */
void browse_ui_opened(void);

void browse_ui_set_queue(const browse_item_t * items, size_t count);
void browse_ui_set_playlists(const browse_item_t * items, size_t count);
/* The tracks of one playlist; switches the playlists tab to the track view with `playlist_title` as header. */
void browse_ui_set_tracks(const char * playlist_title, const browse_item_t * items, size_t count);

#ifdef __cplusplus
} /* extern "C" */
#endif
