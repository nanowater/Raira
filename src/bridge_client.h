#pragma once

#include <memory>
#include <nlohmann/json_fwd.hpp>
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
    static constexpr int DEFAULT_PORT = 18801; /* the bridge's own server (v0 API, settings page) */

    BridgeSource(const std::string & spec, const std::string & token, const std::string & player);

    const char * name() const override
    {
        return "bridge";
    }
    uint32_t capabilities() const override; /* until the first poll: the basics */

    bool get_current(MediaInfo & out) override;
    bool get_cover(const MediaInfo & info, int want_px, std::vector<unsigned char> & out) override;

    bool play_pause() override
    {
        return post("/cmd/playpause", "");
    }
    bool next() override
    {
        return post("/cmd/next", "");
    }
    bool previous() override
    {
        return post("/cmd/previous", "");
    }
    bool toggle_shuffle() override;
    bool toggle_repeat() override;
    bool set_volume(double level) override;
    bool seek_absolute(int seconds) override;
    bool set_endpoint(const std::string & spec) override;
    std::string endpoint() const override;
    bool uses_token() const override
    {
        return true;
    }
    bool can_select_player() const override
    {
        return true;
    }
    bool list_players(std::vector<PlayerInfo> & out, std::string & service_pick) override;
    void set_player(const std::string & id) override
    {
        player_ = id;
    }
    void set_token(const std::string & token) override
    {
        token_ = token;
    }

  private:
    bool post(const std::string & path, const std::string & json_body);
    std::string url(const std::string & path) const
    {
        return cfg_.base_url() + "/v0" + path;
    }
    std::string player_query() const; /* "?player=<id>", or "" for the bridge's own pick */

    TidalConfig cfg_;
    std::unique_ptr<HttpClient> http_;
    std::string token_;
    std::string player_;         /* "" = the bridge's own pick */
    bool shuffle_       = false; /* last polled state: the bridge takes explicit values, not toggles */
    std::string repeat_ = "off";
};

/* BrowseSource for the host bridge's /browse endpoints (its own connection: it runs on the browse thread). */
class BridgeBrowse : public BrowseSource {
  public:
    BridgeBrowse(const std::string & spec, const std::string & token);

    bool list_queue(std::vector<BrowseItem> & out) override;
    bool list_playlists(std::vector<BrowseItem> & out) override;
    bool list_tracks(const std::string & playlist_id, std::string & playlist_title,
                     std::vector<BrowseItem> & out) override;
    bool play_item(const BrowseItem & item) override;
    bool get_thumb(const std::string & cover_key, int px, CoverPixels & out) override;
    bool set_endpoint(const std::string & spec) override;
    void set_token(const std::string & token) override
    {
        token_ = token;
    }

  private:
    bool get_json(const std::string & path, nlohmann::json & out);
    std::string url(const std::string & path) const
    {
        return cfg_.base_url() + "/v0" + path;
    }

    TidalConfig cfg_;
    std::unique_ptr<HttpClient> http_;
    std::string token_;
};
