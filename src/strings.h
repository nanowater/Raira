#pragma once

/* All user-visible text of the UI, in English (the default language). Keeping it in one place makes a
 * later translation a matter of swapping this table (e.g. LVGL's translation support); the fonts already
 * cover Hangul, kana and Han for lyrics/titles. Dynamic parts are appended by the caller. */
namespace str {

/* main screen */
constexpr const char * CONNECTION_LOST = "Connection lost";
constexpr const char * LOADING_LYRICS  = "Loading lyrics...";
constexpr const char * LYRICS_RETRYING = "Retrying lyrics...";
constexpr const char * NO_LYRICS       = "No lyrics found";

/* playlist / queue browser */
constexpr const char * BROWSE_QUEUE     = "Queue";
constexpr const char * BROWSE_PLAYLISTS = "Playlists";
constexpr const char * BROWSE_EMPTY     = "Nothing here";

/* settings: titles and rows */
constexpr const char * SETTINGS      = "Settings";
constexpr const char * MENU_TITLE    = "Menu";
constexpr const char * WIFI          = "WiFi";
constexpr const char * PASSWORD      = "Password";
constexpr const char * PLAYER_ROW    = "Player";
constexpr const char * PLAYER_TITLE  = "Player";
constexpr const char * SOURCES_ROW   = "Sources";
constexpr const char * SOURCES_TITLE = "Sources";
constexpr const char * NOT_SET       = "Not set";
constexpr const char * DISPLAY_ROW   = "Display";
constexpr const char * DISPLAY_TITLE = "Display";
constexpr const char * BRIGHTNESS    = "Brightness";
constexpr const char * VOLUME_BUTTON = "Volume button";
constexpr const char * VOL_AUTO      = "Auto";
constexpr const char * VOL_SHOW      = "Show";
constexpr const char * VOL_HIDE      = "Hide";

/* settings: player choice */
constexpr const char * PLAYER_AUTO     = "Automatic";
constexpr const char * PLAYER_AUTO_SUB = "Follows what is playing";
constexpr const char * PLAYER_LOADING  = "Looking for players...";
constexpr const char * PLAYER_NONE     = "No player is running on the PC";
constexpr const char * PLAYER_FAILED   = "Cannot reach the bridge";

/* settings: sources (programs on a PC that connect to this device) */
constexpr const char * SOURCES_PAIR    = "Pair a source";
constexpr const char * SOURCES_CANCEL  = "Cancel";
constexpr const char * SOURCES_HINT    = "Type the code into the bridge on your PC (settings page > Devices)";
constexpr const char * SOURCES_NONE    = "No source yet. Start the bridge on your PC.";
constexpr const char * SOURCES_WAITING = "Waiting for a code";
constexpr const char * SOURCES_REMOVE  = "Remove";

/* settings: WiFi */
constexpr const char * NOT_CONNECTED  = "Not connected";
constexpr const char * CONNECTING     = "Connecting...";
constexpr const char * CONNECT_FAILED = "Connection failed";
constexpr const char * SCANNING       = "Scanning for WiFi networks...";
constexpr const char * SELECT_NETWORK = "Select a network";
constexpr const char * CONNECTED_TO   = "Connected: ";    /* + ssid */
constexpr const char * CONNECTING_TO  = "Connecting to "; /* + ssid + " ..." */
constexpr const char * CHECK_PASSWORD = "Connection failed. Check the password.";
constexpr const char * TAG_SECURED    = "Secured";
constexpr const char * TAG_OPEN       = "Open";
constexpr const char * TAG_CONNECTED  = "Connected";
constexpr const char * RESCAN         = "Rescan";
constexpr const char * CONNECT        = "Connect";

/* settings: server */

} // namespace str
