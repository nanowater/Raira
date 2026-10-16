#pragma once

#include <functional>
#include <string>
#include "lvgl_compat.h"
#include "platform/wifi.h"

/* Settings overlay: WiFi (scan, pick a network, password with an on-screen keyboard) and the address of
 * the music service. Pure LVGL; it talks to the outside only through this struct. */
struct SettingsServices {
    WifiManager * wifi = nullptr; /* not owned; may be null (the WiFi row is then hidden) */
    std::function<std::string()> get_server;                     /* current endpoint, "host[:port]" */
    std::function<bool(const std::string &)> set_server;         /* validate + apply + persist; false = invalid */
    std::function<void(const std::string & ssid, const std::string & password)> remember_wifi; /* persist */
};

/* Build the (hidden) overlay on the active screen. Call once, after the main UI. LVGL thread only. */
void settings_ui_init(SettingsServices services);

/* Show the overlay and start a WiFi scan. */
void settings_ui_open();
