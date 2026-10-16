#pragma once

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
void media_poller_start(std::unique_ptr<MediaSource> source);

/* Raise (delta > 0) or lower (delta < 0) the volume by `delta` steps of 2 %. Thread-safe and
 * non-blocking; rapid calls are coalesced into one request per 80 ms. Does nothing until the
 * first poll has reported the current volume. This is the hook for the rotary encoder. */
void media_poller_volume_step(int delta);

/* Switch the music service to another endpoint ("host[:port]"). Thread-safe, applied on the poller thread;
 * the next poll uses it. An invalid value is ignored by the source. */
void media_poller_set_endpoint(const std::string & spec);

/* Called on the LVGL thread when the connection state changes (also once right after the endpoint was
 * switched, with UNKNOWN). Set once, e.g. by the settings page; default: none. */
void media_poller_set_connection_cb(void (*cb)(media_conn_state_t state));
