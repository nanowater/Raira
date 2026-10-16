#pragma once

/* Simulator-only stand-in for a real playlist/queue provider (the Spotify backend of the host bridge, see
 * docs/BRIDGE_PROTOCOL.md): canned playlists and a queue that follows what the user taps. It does not
 * touch the real player. Call on the LVGL thread after music_app_start(). */
void fake_browse_install(void);
