#pragma once

#include "tidal_client.h"

/* Starts the background polling thread. Call after music_ui_create() on the LVGL thread.
 * Results reach the UI through a queue drained by an LVGL timer. */
void media_poller_start(const TidalConfig & cfg);

/* Raise (delta > 0) or lower (delta < 0) the volume by `delta` steps of 2 %. Thread-safe and
 * non-blocking; rapid calls are coalesced into one request per 80 ms. Does nothing until the
 * first poll has reported the current volume. This is the hook for the rotary encoder. */
void media_poller_volume_step(int delta);
