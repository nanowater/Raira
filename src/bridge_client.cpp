#include "bridge_client.h"
#include <cstdio>
#include <nlohmann/json.hpp>

using nlohmann::json;

BridgeSource::BridgeSource(const std::string & spec) : http_(http_client_create())
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

bool BridgeSource::post(const std::string & path, const std::string & json_body)
{
    HttpRequest req;
    req.method = "POST";
    req.url    = url(path);
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
    }
    return caps;
}

bool BridgeSource::get_current(MediaInfo & out)
{
    HttpRequest req;
    req.url          = url("/state");
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
    req.url          = url("/cover/" + url_escape(info.image_url) + "?px=" + std::to_string(want_px));
    req.timeout_s    = 10; /* the bridge may have to fetch and re-encode the cover first */
    HttpResponse res = http_->perform(req);
    if(!res.ok() || res.body.empty()) return false;
    out.assign(res.body.begin(), res.body.end());
    return true;
}
