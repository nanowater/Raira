#pragma once

#include <memory>
#include <string>
#include <vector>
#include "media_info.h"
#include "media_source.h"
#include "platform/http.h"

struct TidalConfig {
    std::string host = "localhost";
    int port         = 47836;

    /* Parse "host", "host:port" or "http://host:port". Returns false on bad input. */
    bool parse(const std::string & spec);
    std::string base_url() const;
};

/* MediaSource for the tidal-hifi local HTTP API (TIDAL on the host PC). The only place that knows
 * tidal-hifi's JSON and endpoints. Owns one HttpClient so connections can be reused (keep-alive). */
class TidalClient : public MediaSource {
public:
    explicit TidalClient(TidalConfig cfg) : cfg_(std::move(cfg)), http_(http_client_create()) {}

    const char * name() const override { return "tidal"; }
    uint32_t capabilities() const override
    {
        return MEDIA_CAP_SEEK | MEDIA_CAP_VOLUME | MEDIA_CAP_SHUFFLE | MEDIA_CAP_REPEAT | MEDIA_CAP_PREV_NEXT;
    }

    bool get_current(MediaInfo & out) override;
    /* Prefers the artwork URL from /current (right size from TIDAL's CDN); falls back to /current/image,
     * which can lag behind or show an in-between track's art while skipping. */
    bool get_cover(const MediaInfo & info, int want_px, std::vector<unsigned char> & out) override;

    /* Transport commands; each returns true on HTTP 200. */
    bool play_pause() override { return http_post("/player/playpause"); }
    bool next() override { return http_post("/player/next"); }
    bool previous() override { return http_post("/player/previous"); }
    bool toggle_shuffle() override { return http_post("/player/shuffle/toggle"); }
    bool toggle_repeat() override { return http_post("/player/repeat/toggle"); }
    bool set_volume(double level) override; /* 0.0..1.0 */
    bool seek_absolute(int seconds) override { return http_put("/player/seek/absolute?seconds=" + std::to_string(seconds)); }

private:
    bool fetch_url(const std::string & url, std::vector<unsigned char> & out);
    bool fetch_current_image(std::vector<unsigned char> & out);
    bool http_post(const std::string & path);
    bool http_put(const std::string & path);
    bool http_send(const char * method, const std::string & path);
    bool http_get(const std::string & path, std::string & body);
    TidalConfig cfg_;
    std::unique_ptr<HttpClient> http_;
};
