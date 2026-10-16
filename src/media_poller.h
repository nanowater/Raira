#pragma once

#include <memory>
#include "media_source.h"

/* Starts the background threads and hands them the music service to poll and control. The poller thread
 * becomes the only user of `source`. Call after music_ui_create() on the LVGL thread; results reach the UI
 * through a queue drained by an LVGL timer. */
void media_poller_start(std::unique_ptr<MediaSource> source);

/* Raise (delta > 0) or lower (delta < 0) the volume by `delta` steps of 2 %. Thread-safe and
 * non-blocking; rapid calls are coalesced into one request per 80 ms. Does nothing until the
 * first poll has reported the current volume. This is the hook for the rotary encoder. */
void media_poller_volume_step(int delta);
