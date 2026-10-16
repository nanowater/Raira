#pragma once

#include <functional>
#include <memory>
#include <string>
#include "media_source.h"

/* Whether the poller reaches the music service, as seen in its own poll loop. UNKNOWN is the window
 * right after the endpoint was switched, before a poll could confirm the new address. */
typedef enum {
    MEDIA_CONN_CONNECTED,
    MEDIA_CONN_LOST,
    MEDIA_CONN_UNKNOWN
} media_conn_state_t;

/* Starts the background threads and hands them the music service to poll and control. The poller thread
 * becomes the only user of `source`. Call after music_ui_create() on the LVGL thread; results reach the UI
 * through a queue drained by an LVGL timer. */
/* Whether the browse button can ever show: NONE = no BrowseSource (the bit is always cleared), WHEN_REPORTED =
 * only while the source's capabilities include MEDIA_CAP_BROWSE, ALWAYS = the browse provider does not depend on
 * the player (the simulator's fake data). */
enum media_browse_mode_t { MEDIA_BROWSE_NONE, MEDIA_BROWSE_WHEN_REPORTED, MEDIA_BROWSE_ALWAYS };

void media_poller_start(std::unique_ptr<MediaSource> source, media_browse_mode_t browse = MEDIA_BROWSE_NONE);

/* Run `fn` on the LVGL thread (from any thread): queued, drained by an LVGL timer. The only safe way for a
 * background thread to touch the UI. */
void media_post_to_ui(std::function<void()> fn);

/* Raise (delta > 0) or lower (delta < 0) the volume by `delta` steps of 2 %. Thread-safe and
 * non-blocking; rapid calls are coalesced into one request per 80 ms. Does nothing until the
 * first poll has reported the current volume, or when the service reports no MEDIA_CAP_VOLUME.
 * This is the hook for the rotary encoder. */
void media_poller_volume_step(int delta);

/* Switch the music service to another endpoint ("host[:port]"). Thread-safe, applied on the poller thread;
 * the next poll uses it. An invalid value is ignored by the source. */
void media_poller_set_endpoint(const std::string & spec);

/* Give the music service another access token (see MediaSource::set_token). Thread-safe, applied on the poller
 * thread; the next poll uses it and the connection state goes UNKNOWN until that poll decides. */
void media_poller_set_token(const std::string & token);

/* Called on the LVGL thread when the connection state changes (also once right after the endpoint was
 * switched, with UNKNOWN). Set once, e.g. by the settings page; default: none. */
void media_poller_set_connection_cb(void (*cb)(media_conn_state_t state));
