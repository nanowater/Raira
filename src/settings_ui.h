#pragma once

#include <functional>
#include <string>
#include "lvgl_compat.h"
#include "hub_source.h"
#include "media_poller.h"
#include "platform/wifi.h"

/* Settings overlay: WiFi (scan, pick a network, password with an on-screen keyboard) and the address of
 * the music service. Pure LVGL; it talks to the outside only through this struct. */
struct SettingsServices {
    HubSource * hub = nullptr;    /* not owned: sources can pair with this device; adds the Sources page */
    WifiManager * wifi = nullptr; /* not owned; may be null (the WiFi row is then hidden) */
    std::function<std::string()> get_server;                     /* current endpoint, "host[:port]" */
    std::function<bool(const std::string &)> set_server;         /* validate + apply + persist; false = invalid */
    std::function<void(const std::string & ssid, const std::string & password)> remember_wifi; /* persist */
    /* Access token of the music service (the host bridge): both unset = the service needs none, the row is hidden. */
    std::function<std::string()> get_token;
    std::function<void(const std::string &)> set_token;
    /* Player choice of the music service (several players on the host): both unset = nothing to choose, the row is
     * hidden. "" means automatic. */
    std::function<std::string()> get_player;
    std::function<void(const std::string &)> set_player;         /* apply + persist */
    std::function<void(media_players_cb_t)> list_players;        /* ask the service; the answer comes on the LVGL thread */
    /* (applied right below) */           /* apply + persist; "" clears it */
};

/* Build the (hidden) overlay on the active screen. Call once, after the main UI. LVGL thread only. */
void settings_ui_init(SettingsServices services);

/* Show the overlay on its settings page. */
void settings_ui_open();

/* Show the overlay on its menu (long press on the cover): quick access to the player choice and to the settings. Back from either
 * returns to the menu, back from the menu closes the overlay. */
void settings_ui_open_menu();
