#pragma once

#include <string>

/* Service-agnostic now-playing model. UI/poller code depends on this,
 * not on any backend's JSON (tidal-hifi, Spotify, ...). */
struct MediaInfo {
    std::string title;
    std::string artists;
    std::string album;
    std::string status;  /* "playing" / "paused" / ... */
    std::string track_id; /* stable key for change detection (url, else title) */
    std::string image_url; /* cover URL; changes when the artwork changes */
    int current_sec  = 0;
    int duration_sec = 0;
};
