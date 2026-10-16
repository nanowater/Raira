#include "bridge_client.h"
#include <cstdio>
#include <nlohmann/json.hpp>

using nlohmann::json;

static std::string str_of(const json & j, const char * key);

BridgeSource::BridgeSource(const std::string & spec, const std::string & token, const std::string & player)
    : http_(http_client_create()), token_(token), player_(player)
{
    cfg_.port = DEFAULT_PORT;
    if(!spec.empty()) cfg_.parse(spec);
}

uint32_t BridgeSource::capabilities() const
{
    return MEDIA_CAP_SEEK | MEDIA_CAP_VOLUME | MEDIA_CAP_SHUFFLE | MEDIA_CAP_REPEAT | MEDIA_CAP_PREV_NEXT;
}

bool BridgeSource::set_endpoint(const std::string & spec)
{
    TidalConfig c = cfg_;
    if(!c.parse(spec)) return false;
    cfg_ = c;
    return true;
}

std::string BridgeSource::endpoint() const
{
    return cfg_.port == DEFAULT_PORT ? cfg_.host : cfg_.host + ":" + std::to_string(cfg_.port);
}

std::string BridgeSource::player_query() const
{
    return player_.empty() ? "" : "?player=" + url_escape(player_);
}

bool BridgeSource::list_players(std::vector<PlayerInfo> & out, std::string & service_pick)
{
    HttpRequest req;
    req.bearer_token = token_;
    req.url          = url("/players");
    HttpResponse res = http_->perform(req);
    if(!res.ok() || res.body.empty()) return false;
    json j = json::parse(res.body, nullptr, false);
    if(j.is_discarded() || !j.is_object()) return false;
    service_pick = j.contains("selected") && j["selected"].is_string() ? j["selected"].get<std::string>() : "";
    out.clear();
    auto list = j.find("players");
    if(list == j.end() || !list->is_array()) return true;
    for(const auto & p : *list) {
        if(!p.is_object()) continue;
        out.push_back({str_of(p, "id"), str_of(p, "name"), str_of(p, "service"), str_of(p, "status")});
    }
    return true;
}

bool BridgeSource::post(const std::string & path, const std::string & json_body)
{
    HttpRequest req;
    req.bearer_token = token_;
    req.method = "POST";
    req.url    = url(path) + player_query();
    if(!json_body.empty()) {
        req.body         = json_body;
        req.content_type = "application/json";
    }
    return http_->perform(req).ok();
}

bool BridgeSource::toggle_shuffle()
{
    return post("/cmd/shuffle", std::string("{\"enabled\":") + (shuffle_ ? "false" : "true") + "}");
}

bool BridgeSource::toggle_repeat()
{
    /* cycles off -> all -> single -> off, like the buttons expect */
    const char * next = repeat_ == "off" ? "all" : repeat_ == "all" ? "single" : "off";
    return post("/cmd/repeat", std::string("{\"mode\":\"") + next + "\"}");
}

bool BridgeSource::set_volume(double level)
{
    if(level < 0) level = 0;
    if(level > 1) level = 1;
    char b[48];
    snprintf(b, sizeof(b), "{\"volume\":%.2f}", level);
    return post("/cmd/volume", b);
}

bool BridgeSource::seek_absolute(int seconds)
{
    return post("/cmd/seek", "{\"position_ms\":" + std::to_string((long long)seconds * 1000) + "}");
}

static std::string str_of(const json & j, const char * key)
{
    auto it = j.find(key);
    return (it != j.end() && it->is_string()) ? it->get<std::string>() : std::string();
}

static uint32_t caps_of(const json & j)
{
    uint32_t caps = 0;
    auto it       = j.find("capabilities");
    if(it == j.end() || !it->is_array()) return caps;
    for(const auto & c : *it) {
        if(!c.is_string()) continue;
        const std::string s = c.get<std::string>();
        if(s == "seek") caps |= MEDIA_CAP_SEEK;
        else if(s == "volume") caps |= MEDIA_CAP_VOLUME;
        else if(s == "shuffle") caps |= MEDIA_CAP_SHUFFLE;
        else if(s == "repeat") caps |= MEDIA_CAP_REPEAT;
        else if(s == "prev_next") caps |= MEDIA_CAP_PREV_NEXT;
        else if(s == "browse_playlists" || s == "browse_queue") caps |= MEDIA_CAP_BROWSE;
    }
    return caps;
}

bool BridgeSource::get_current(MediaInfo & out)
{
    HttpRequest req;
    req.bearer_token = token_;
    req.url          = url("/state") + player_query();
    HttpResponse res = http_->perform(req);
    if(!res.transport_ok) return false;

    MediaInfo m;
    if(res.status == 404) {
        /* The bridge is reachable but has no player: an idle state, not a lost connection. */
        m.status = "stopped";
        m.caps   = 0;
        out      = std::move(m);
        return true;
    }
    if(!res.ok() || res.body.empty()) return false;
    json j = json::parse(res.body, nullptr, false);
    if(j.is_discarded() || !j.is_object()) return false;

    m.status = str_of(j, "status");
    auto tr  = j.find("track");
    if(tr != j.end() && tr->is_object()) {
        m.title = str_of(*tr, "title");
        m.album = str_of(*tr, "album");
        auto ar = tr->find("artists");
        if(ar != tr->end() && ar->is_array()) {
            for(const auto & a : *ar) {
                if(!a.is_string()) continue;
                if(!m.artists.empty()) m.artists += ", ";
                m.artists += a.get<std::string>();
            }
        }
        auto d = tr->find("duration_ms");
        if(d != tr->end() && d->is_number()) m.duration_sec = (int)(d->get<double>() / 1000.0);
        m.track_id  = str_of(*tr, "id");
        m.image_url = str_of(*tr, "cover"); /* opaque cover key; get_cover() turns it into a request */
    }
    auto pos = j.find("position_ms");
    if(pos != j.end() && pos->is_number()) m.current_sec = (int)(pos->get<double>() / 1000.0);
    auto vol = j.find("volume");
    if(vol != j.end() && vol->is_number()) m.volume = vol->get<double>();
    auto sh = j.find("shuffle");
    if(sh != j.end() && sh->is_boolean()) m.shuffle = sh->get<bool>();
    m.repeat = str_of(j, "repeat");
    auto q   = j.find("quality");
    if(q != j.end() && q->is_object()) {
        m.quality       = str_of(*q, "label");
        m.quality_badge = str_of(*q, "badge");
    }
    m.caps = caps_of(j);

    shuffle_ = m.shuffle;
    repeat_  = m.repeat.empty() ? "off" : m.repeat;
    out      = std::move(m);
    return true;
}

bool BridgeSource::get_cover(const MediaInfo & info, int want_px, std::vector<unsigned char> & out)
{
    if(info.image_url.empty()) return false;
    HttpRequest req;
    req.bearer_token = token_;
    req.url          = url("/cover/" + url_escape(info.image_url) + "?px=" + std::to_string(want_px));
    req.timeout_s    = 10; /* the bridge may have to fetch and re-encode the cover first */
    HttpResponse res = http_->perform(req);
    if(!res.ok() || res.body.empty()) return false;
    out.assign(res.body.begin(), res.body.end());
    return true;
}

/* ---- Browse ---- */

BridgeBrowse::BridgeBrowse(const std::string & spec, const std::string & token) : http_(http_client_create()), token_(token)
{
    cfg_.port = BridgeSource::DEFAULT_PORT;
    if(!spec.empty()) cfg_.parse(spec);
}

bool BridgeBrowse::set_endpoint(const std::string & spec)
{
    TidalConfig c = cfg_;
    if(!c.parse(spec)) return false;
    cfg_ = c;
    return true;
}

bool BridgeBrowse::get_json(const std::string & path, json & out)
{
    HttpRequest req;
    req.bearer_token = token_;
    req.url       = url(path);
    req.timeout_s = 10; /* the bridge may be waiting for the service's web API */
    HttpResponse res = http_->perform(req);
    if(!res.ok() || res.body.empty()) return false;
    out = json::parse(res.body, nullptr, false);
    return !out.is_discarded() && out.is_object();
}

static std::string join_artists(const json & j)
{
    std::string out;
    auto ar = j.find("artists");
    if(ar == j.end() || !ar->is_array()) return out;
    for(const auto & a : *ar) {
        if(!a.is_string()) continue;
        if(!out.empty()) out += ", ";
        out += a.get<std::string>();
    }
    return out;
}

/* A track entry of /browse/queue or /browse/playlists/<id>/tracks. `fallback_context` is the playlist the
 * list belongs to (queue entries carry their own, possibly null). */
static BrowseItem track_item(const json & t, const std::string & fallback_context)
{
    BrowseItem it;
    it.id       = str_of(t, "id");
    it.title    = str_of(t, "title");
    it.subtitle = join_artists(t);
    it.cover    = str_of(t, "cover");
    it.context  = str_of(t, "context");
    if(it.context.empty()) it.context = fallback_context;
    auto idx = t.find("index");
    if(idx != t.end() && idx->is_number()) it.index = idx->get<int>();
    it.playable = !it.context.empty() && it.index >= 0;
    return it;
}

bool BridgeBrowse::list_queue(std::vector<BrowseItem> & out)
{
    json j;
    if(!get_json("/browse/queue", j)) return false;
    out.clear();
    auto now = j.find("now");
    if(now != j.end() && now->is_object()) {
        BrowseItem it = track_item(*now, "");
        it.current    = true;
        out.push_back(std::move(it));
    }
    auto items = j.find("items");
    if(items != j.end() && items->is_array())
        for(const auto & t : *items)
            if(t.is_object()) out.push_back(track_item(t, ""));
    return true;
}

bool BridgeBrowse::list_playlists(std::vector<BrowseItem> & out)
{
    json j;
    if(!get_json("/browse/playlists", j)) return false;
    out.clear();
    auto items = j.find("items");
    if(items == j.end() || !items->is_array()) return true;
    for(const auto & p : *items) {
        if(!p.is_object()) continue;
        BrowseItem it;
        it.id    = str_of(p, "id");
        it.title = str_of(p, "name");
        auto n   = p.find("track_count");
        if(n != p.end() && n->is_number()) it.subtitle = std::to_string(n->get<int>()) + (n->get<int>() == 1 ? " track" : " tracks");
        it.cover    = str_of(p, "cover");
        it.playable = true; /* opening it is always possible */
        out.push_back(std::move(it));
    }
    return true;
}

bool BridgeBrowse::list_tracks(const std::string & playlist_id, std::string & playlist_title, std::vector<BrowseItem> & out)
{
    (void)playlist_title; /* the caller already has the name from the playlist list */
    out.clear();
    int offset = 0;
    /* A page at a time (the bridge caps it); a few pages are plenty for a 5" screen. */
    for(int page = 0; page < 4; page++) {
        json j;
        if(!get_json("/browse/playlists/" + url_escape(playlist_id) + "/tracks?limit=50&offset=" + std::to_string(offset), j))
            return page > 0;
        auto items = j.find("items");
        if(items == j.end() || !items->is_array()) break;
        for(const auto & t : *items)
            if(t.is_object()) out.push_back(track_item(t, playlist_id));
        auto next = j.find("next");
        if(next == j.end() || !next->is_number()) break;
        offset = next->get<int>();
    }
    return true;
}

bool BridgeBrowse::play_item(const BrowseItem & item)
{
    HttpRequest req;
    req.bearer_token = token_;
    req.method       = "POST";
    req.url          = url("/cmd/play_item");
    req.body         = json{{"context", item.context}, {"index", item.index}}.dump();
    req.content_type = "application/json";
    return http_->perform(req).ok();
}

bool BridgeBrowse::get_thumb(const std::string & cover_key, int px, CoverPixels & out)
{
    HttpRequest req;
    req.bearer_token = token_;
    req.url          = url("/cover/" + url_escape(cover_key) + "?px=" + std::to_string(px));
    req.timeout_s    = 10;
    HttpResponse res = http_->perform(req);
    if(!res.ok() || res.body.empty()) return false;
    std::vector<unsigned char> jpeg(res.body.begin(), res.body.end());
    return cover_decode(jpeg, (uint32_t)px, out);
}
