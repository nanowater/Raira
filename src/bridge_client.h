#pragma once

#include <memory>
#include <string>
#include <vector>
#include "media_source.h"
#include "platform/http.h"
#include "tidal_client.h" /* TidalConfig: the "host[:port]" parser (generic despite the name) */

/* MediaSource for the host bridge (bridge/, protocol in docs/BRIDGE_PROTOCOL.md): one connection to a
 * program on the PC that normalises the players there (TIDAL, Spotify, browsers, ...). The only place that
 * knows the bridge's JSON. What the selected player can do changes at runtime, so every poll carries the
 * capabilities (MediaInfo::caps). */
class BridgeSource : public MediaSource {
public:
    static constexpr int DEFAULT_PORT = 47900;

    explicit BridgeSource(const std::string & spec);

    const char * name() const override { return "bridge"; }
    uint32_t capabilities() const override; /* until the first poll: the basics */

    bool get_current(MediaInfo & out) override;
    bool get_cover(const MediaInfo & info, int want_px, std::vector<unsigned char> & out) override;

    bool play_pause() override { return post("/cmd/playpause", ""); }
    bool next() override { return post("/cmd/next", ""); }
    bool previous() override { return post("/cmd/previous", ""); }
    bool toggle_shuffle() override;
    bool toggle_repeat() override;
    bool set_volume(double level) override;
    bool seek_absolute(int seconds) override;
    bool set_endpoint(const std::string & spec) override;
    std::string endpoint() const override;

private:
    bool post(const std::string & path, const std::string & json_body);
    std::string url(const std::string & path) const { return cfg_.base_url() + "/v0" + path; }

    TidalConfig cfg_;
    std::unique_ptr<HttpClient> http_;
    bool shuffle_ = false;        /* last polled state: the bridge takes explicit values, not toggles */
    std::string repeat_ = "off";
};
