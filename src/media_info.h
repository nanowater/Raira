#pragma once

#include <string>

/* Service-agnostic now-playing model. UI/poller code depends on this,
 * not on any backend's JSON (tidal-hifi, Spotify, ...). */
struct MediaInfo {
    std::string title;
    std::string artists;
    std::string album;
    std::string quality; /* e.g. "HI_RES_LOSSLESS", "LOSSLESS" */
    std::string quality_badge; /* e.g. "24-bit 48.0kHz" */
    bool shuffle = false;
    std::string repeat;  /* "off" / "single" / "all" */
    std::string status;  /* "playing" / "paused" / ... */
    std::string track_id; /* stable key for change detection (url, else title) */
    std::string image_url; /* cover URL; changes when the artwork changes */
    double volume = -1;   /* 0.0..1.0, -1 when the service did not report it */
    int current_sec  = 0;
    int duration_sec = 0;
};
