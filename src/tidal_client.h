#pragma once

#include <string>
#include <vector>
#include "media_info.h"

struct TidalConfig {
    std::string host = "localhost";
    int port         = 47836;

    /* Parse "host", "host:port" or "http://host:port". Returns false on bad input. */
    bool parse(const std::string & spec);
    std::string base_url() const;
};

/* Blocking client for the tidal-hifi local HTTP API. Never call from the LVGL thread. */
class TidalClient {
public:
    explicit TidalClient(TidalConfig cfg) : cfg_(std::move(cfg)) {}

    bool get_current(MediaInfo & out);
    /* Raw encoded image bytes (JPEG/PNG) of the current cover. */
    bool get_cover(std::vector<unsigned char> & out);

    /* Transport commands; each returns true on HTTP 200. */
    bool play_pause() { return http_post("/player/playpause"); }
    bool next() { return http_post("/player/next"); }
    bool previous() { return http_post("/player/previous"); }

private:
    bool http_post(const std::string & path);
    bool http_get(const std::string & path, std::string & body);
    TidalConfig cfg_;
};
