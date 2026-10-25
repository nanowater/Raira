#pragma once

#include <memory>
#include <string>
#include "media_source.h"

/* Runs the playlist/queue browser: takes the panel's requests (music_ui_set_browse_cb, thumbnails) on its own
 * thread, calls `source`, and posts the lists back to the LVGL thread. The worker thread becomes the only
 * user of `source`. Call once on the LVGL thread after music_ui_create() and media_poller_start(). */
void browse_worker_start(std::unique_ptr<BrowseSource> source);
