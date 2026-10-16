#pragma once

/* All user-visible text of the UI, in English (the default language). Keeping it in one place makes a
 * later translation a matter of swapping this table (e.g. LVGL's translation support); the fonts already
 * cover Hangul, kana and Han for lyrics/titles. Dynamic parts are appended by the caller. */
namespace str {

/* main screen */
constexpr const char * CONNECTION_LOST = "Connection lost";
constexpr const char * LOADING_LYRICS  = "Loading lyrics...";

/* playlist / queue browser */
constexpr const char * BROWSE_QUEUE     = "Queue";
constexpr const char * BROWSE_PLAYLISTS = "Playlists";
constexpr const char * BROWSE_EMPTY     = "Nothing here";

/* settings: titles and rows */
constexpr const char * SETTINGS        = "Settings";
constexpr const char * WIFI            = "WiFi";
constexpr const char * PASSWORD        = "Password";
constexpr const char * SERVER_TITLE    = "Server address";
constexpr const char * SERVER_ROW      = "Server (tidal-hifi)";
constexpr const char * NOT_SET         = "Not set";

/* settings: WiFi */
constexpr const char * NOT_CONNECTED   = "Not connected";
constexpr const char * CONNECTING      = "Connecting...";
constexpr const char * CONNECT_FAILED  = "Connection failed";
constexpr const char * SCANNING        = "Scanning for WiFi networks...";
constexpr const char * SELECT_NETWORK  = "Select a network";
constexpr const char * CONNECTED_TO    = "Connected: ";       /* + ssid */
constexpr const char * CONNECTING_TO   = "Connecting to ";    /* + ssid + " ..." */
constexpr const char * CHECK_PASSWORD  = "Connection failed. Check the password.";
constexpr const char * TAG_SECURED     = "Secured";
constexpr const char * TAG_OPEN        = "Open";
constexpr const char * TAG_CONNECTED   = "Connected";
constexpr const char * RESCAN          = "Rescan";
constexpr const char * CONNECT         = "Connect";

/* settings: server */
constexpr const char * SERVER_HINT     = "Address of the PC running tidal-hifi (e.g. 192.168.0.10 or 192.168.0.10:47836)";
constexpr const char * SERVER_INVALID  = "Invalid address (e.g. 192.168.0.10:47836)";
constexpr const char * SERVER_PLACEHOLDER = "host[:port]";
constexpr const char * SAVE            = "Save";
constexpr const char * SERVER_CHECKING   = "Saved. Checking the connection...";
constexpr const char * SERVER_CONNECTED  = "Connected";
constexpr const char * SERVER_UNREACHABLE = "Not connected";

} // namespace str
