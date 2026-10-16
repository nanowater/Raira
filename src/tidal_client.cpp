#include "tidal_client.h"
#include <nlohmann/json.hpp>

using nlohmann::json;

bool TidalConfig::parse(const std::string & spec)
{
    std::string s = spec;
    const std::string scheme = "http://";
    if(s.compare(0, scheme.size(), scheme) == 0) s.erase(0, scheme.size());
    while(!s.empty() && s.back() == '/') s.pop_back();
    if(s.empty()) return false;

    size_t colon = s.rfind(':');
    if(colon == std::string::npos) {
        host = s;
        return true;
    }
    if(colon == 0) return false;
    int p = atoi(s.c_str() + colon + 1);
    if(p <= 0 || p > 65535) return false;
    host = s.substr(0, colon);
    port = p;
    return true;
}

std::string TidalConfig::base_url() const
{
    return "http://" + host + ":" + std::to_string(port);
}

bool TidalClient::http_get(const std::string & path, std::string & body)
{
    HttpRequest req;
    req.url = cfg_.base_url() + path;
    HttpResponse res = http_->perform(req);
    body             = std::move(res.body);
    return res.ok() && !body.empty();
}

bool TidalClient::set_volume(double level)
{
    if(level < 0) level = 0;
    if(level > 1) level = 1;
    char q[48];
    snprintf(q, sizeof(q), "/player/volume?volume=%.2f", level);
    return http_put(q);
}

bool TidalClient::http_post(const std::string & path)
{
    return http_send("POST", path);
}

bool TidalClient::http_put(const std::string & path)
{
    return http_send("PUT", path);
}

/* Body-less request; the parameters of the tidal-hifi player API travel in the query string. */
bool TidalClient::http_send(const char * method, const std::string & path)
{
    HttpRequest req;
    req.method = method;
    req.url    = cfg_.base_url() + path;
    return http_->perform(req).ok();
}

static std::string str_of(const json & j, const char * key)
{
    auto it = j.find(key);
    return (it != j.end() && it->is_string()) ? it->get<std::string>() : std::string();
}

/* Accepts a number or a numeric string ("123"); returns false if neither. */
static bool sec_of(const json & j, const char * key, int & out)
{
    auto it = j.find(key);
    if(it == j.end()) return false;
    if(it->is_number()) {
        out = static_cast<int>(it->get<double>());
        return true;
    }
    if(it->is_string()) {
        try {
            out = std::stoi(it->get<std::string>());
            return true;
        } catch(...) {
        }
    }
    return false;
}

bool TidalClient::get_current(MediaInfo & out)
{
    std::string body;
    if(!http_get("/current", body)) return false;

    json j = json::parse(body, nullptr, false);
    if(j.is_discarded() || !j.is_object()) return false;

    MediaInfo m;
    m.title = str_of(j, "title");
    if(m.title.empty() && j.contains("track") && j["track"].is_object()) m.title = str_of(j["track"], "title");

    /* tidal-hifi sends "artists" as a joined string plus "artistsArray"; other
     * versions send an array under "artists". Accept all of them. */
    for(const char * key : {"artistsArray", "artists"}) {
        auto ar = j.find(key);
        if(ar == j.end()) continue;
        if(ar->is_array()) {
            for(const auto & a : *ar) {
                if(!a.is_string()) continue;
                if(!m.artists.empty()) m.artists += ", ";
                m.artists += a.get<std::string>();
            }
        } else if(ar->is_string()) {
            m.artists = ar->get<std::string>();
        }
        if(!m.artists.empty()) break;
    }
    if(m.artists.empty()) m.artists = str_of(j, "artist");

    m.album  = str_of(j, "album");
    m.status = str_of(j, "status");
    if(!sec_of(j, "currentInSeconds", m.current_sec)) sec_of(j, "current", m.current_sec);
    if(!sec_of(j, "durationInSeconds", m.duration_sec)) sec_of(j, "duration", m.duration_sec);

    auto pl = j.find("player");
    if(pl != j.end() && pl->is_object()) {
        auto sh = pl->find("shuffle");
        if(sh != pl->end() && sh->is_boolean()) m.shuffle = sh->get<bool>();
        m.repeat = str_of(*pl, "repeat");
    }
    auto aq = j.find("audioQuality");
    if(aq != j.end() && aq->is_object()) {
        m.quality       = str_of(*aq, "quality");
        m.quality_badge = str_of(*aq, "badgeText");
    }
    auto vol = j.find("volume");
    if(vol != j.end() && vol->is_number()) m.volume = vol->get<double>();
    m.image_url = str_of(j, "image");
    m.track_id  = str_of(j, "url");
    if(m.track_id.empty()) m.track_id = m.title;

    out = std::move(m);
    return true;
}

bool TidalClient::get_cover_from_url(const std::string & url, std::vector<unsigned char> & out)
{
    HttpRequest req;
    req.url          = url;
    HttpResponse res = http_->perform(req);
    if(!res.ok() || res.body.empty()) return false;
    out.assign(res.body.begin(), res.body.end());
    return true;
}

bool TidalClient::get_cover(std::vector<unsigned char> & out)
{
    std::string body;
    if(!http_get("/current/image", body)) return false;
    out.assign(body.begin(), body.end());
    return true;
}
