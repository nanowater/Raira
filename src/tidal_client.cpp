#include "tidal_client.h"
#include <curl/curl.h>
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

static size_t write_cb(void * ptr, size_t size, size_t nmemb, void * stream)
{
    static_cast<std::string *>(stream)->append(static_cast<char *>(ptr), size * nmemb);
    return size * nmemb;
}

bool TidalClient::http_get(const std::string & path, std::string & body)
{
    CURL * curl = curl_easy_init();
    if(!curl) return false;
    std::string url = cfg_.base_url() + path;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 2L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    CURLcode rc = curl_easy_perform(curl);
    long code   = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    curl_easy_cleanup(curl);
    return rc == CURLE_OK && code == 200 && !body.empty();
}

bool TidalClient::http_post(const std::string & path)
{
    CURL * curl = curl_easy_init();
    if(!curl) return false;
    std::string url  = cfg_.base_url() + path;
    std::string sink;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, "");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &sink);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 2L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    CURLcode rc = curl_easy_perform(curl);
    long code   = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    curl_easy_cleanup(curl);
    return rc == CURLE_OK && code == 200;
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

    m.track_id = str_of(j, "url");
    if(m.track_id.empty()) m.track_id = m.title;

    out = std::move(m);
    return true;
}

bool TidalClient::get_cover(std::vector<unsigned char> & out)
{
    std::string body;
    if(!http_get("/current/image", body)) return false;
    out.assign(body.begin(), body.end());
    return true;
}
