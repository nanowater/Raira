#pragma once

#include <memory>
#include "media_source.h"

/* Portable app entry: builds the UI and starts the background threads with the given music service
 * (e.g. a TidalClient). Call once, on the LVGL thread,
 * after LVGL and the display/input drivers are initialised. Used by the simulator's main() and by the
 * ESP32 app_main alike. */
void music_app_start(std::unique_ptr<MediaSource> source);
