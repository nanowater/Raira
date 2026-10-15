#pragma once

#include <memory>
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

/* Blocking client for the tidal-hifi local HTTP API. Never call from the LVGL thread.
 * Keeps one curl handle so connections are reused (keep-alive); use it from a single thread. */
class TidalClient {
public:
    explicit TidalClient(TidalConfig cfg) : cfg_(std::move(cfg)) {}

    bool get_current(MediaInfo & out);
    /* Raw encoded image bytes (JPEG/PNG) of the current cover. */
    bool get_cover(std::vector<unsigned char> & out);
    /* Cover fetched straight from the artwork URL in MediaInfo::image_url (any http(s) URL). */
    bool get_cover_from_url(const std::string & url, std::vector<unsigned char> & out);

    /* Transport commands; each returns true on HTTP 200. */
    bool play_pause() { return http_post("/player/playpause"); }
    bool next() { return http_post("/player/next"); }
    bool previous() { return http_post("/player/previous"); }

private:
    bool http_post(const std::string & path);
    bool http_get(const std::string & path, std::string & body);
    TidalConfig cfg_;
    std::shared_ptr<void> curl_; /* CURL *, created on first use */
};
