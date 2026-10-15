#pragma once

#include "tidal_client.h"

/* Starts the background polling thread. Call after music_ui_create().
 * Results are pushed to the UI through lv_async_call. */
void media_poller_start(const TidalConfig & cfg);
