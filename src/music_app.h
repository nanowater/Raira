#pragma once

#include <functional>
#include <memory>
#include <string>
#include "media_source.h"
#include "platform/settings.h"
#include "platform/wifi.h"

/* Everything the portable app needs from its host program (simulator main() or ESP32 app_main). */
struct MusicAppDeps {
    std::unique_ptr<MediaSource> source;     /* the music service; the poller thread takes it over */
    std::unique_ptr<BrowseSource> browse;    /* playlists/queue; may be null: no browse button. The browse thread takes it over */
    std::unique_ptr<WifiManager> wifi;       /* may be null: the settings page hides its WiFi row */
    std::unique_ptr<SettingsStore> store;    /* may be null: settings are not persisted */
    AppSettings settings;                    /* current values (server = the endpoint the source starts with) */
    std::function<bool(const std::string &)> validate_server; /* is "host[:port]" acceptable? */
};

/* Portable app entry: builds the UI (incl. the settings page) and starts the background threads. Call
 * once, on the LVGL thread, after LVGL and the display/input drivers are initialised. Used by the
 * simulator's main() and by the ESP32 app_main alike. */
void music_app_start(MusicAppDeps deps);
