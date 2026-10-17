#pragma once

#include <cstdint>
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
    /* Position in milliseconds when the source knows it that precisely (the hub, the bridge), else -1 and only
     * current_sec (whole seconds) is valid. The lyrics clock uses it to stay within a few hundred ms instead of +-1 s. */
    int64_t position_ms = -1;
    int duration_sec = 0;
    /* MediaCapability bits the service supports right now, -1 = not reported (the source's capabilities()
     * stands). A host bridge fills it: what the selected player can do changes when the player does. */
    int64_t caps = -1;
};
