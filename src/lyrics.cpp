#include "lyrics.h"
#include <algorithm>
#include <curl/curl.h>
#include <nlohmann/json.hpp>

using nlohmann::json;

static size_t write_cb(void * ptr, size_t size, size_t nmemb, void * stream)
{
    static_cast<std::string *>(stream)->append(static_cast<char *>(ptr), size * nmemb);
    return size * nmemb;
}

static bool http_get_url(const std::string & url, std::string & body)
{
    CURL * curl = curl_easy_init();
    if(!curl) return false;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "music-controller/0.1 (https://github.com/nanowater)");
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 4L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    CURLcode rc = curl_easy_perform(curl);
    long code   = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    curl_easy_cleanup(curl);
    return rc == CURLE_OK && code == 200 && !body.empty();
}

static std::string escape(const std::string & s)
{
    char * e = curl_easy_escape(nullptr, s.c_str(), (int)s.size());
    std::string out = e ? e : "";
    curl_free(e);
    return out;
}

static std::string trim(const std::string & s)
{
    size_t a = s.find_first_not_of(" \t\r\n");
    if(a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

Lyrics lyrics_parse_lrc(const std::string & lrc)
{
    Lyrics out;
    out.synced = true;

    size_t pos = 0;
    while(pos <= lrc.size()) {
        size_t nl        = lrc.find('\n', pos);
        std::string line = lrc.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        pos              = nl == std::string::npos ? lrc.size() + 1 : nl + 1;

        std::vector<int> stamps;
        size_t i = 0;
        while(i < line.size() && line[i] == '[') {
            size_t close = line.find(']', i);
            if(close == std::string::npos) break;
            int mm = 0, ss = 0, frac = 0;
            char fr[8] = {0};
            /* [mm:ss] or [mm:ss.xx]; tags like [ar:Artist] fail to match and are skipped */
            int n = sscanf(line.c_str() + i, "[%d:%d.%7[0-9]]", &mm, &ss, fr);
            if(n >= 2) {
                if(n == 3) {
                    std::string f = std::string(fr).substr(0, 3);
                    while(f.size() < 3) f += '0';
                    frac = atoi(f.c_str());
                }
                stamps.push_back((mm * 60 + ss) * 1000 + frac);
            }
            i = close + 1;
        }
        if(stamps.empty()) continue;
        std::string text = trim(line.substr(i));
        if(text.empty()) continue;
        for(int t : stamps) out.lines.push_back({t, text});
    }
    std::stable_sort(out.lines.begin(), out.lines.end(),
                     [](const LyricLine & a, const LyricLine & b) { return a.time_ms < b.time_ms; });
    return out;
}

Lyrics lyrics_parse_plain(const std::string & text)
{
    Lyrics out;
    size_t pos = 0;
    while(pos <= text.size()) {
        size_t nl        = text.find('\n', pos);
        std::string line = trim(text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos));
        pos              = nl == std::string::npos ? text.size() + 1 : nl + 1;
        if(!line.empty()) out.lines.push_back({0, line});
    }
    return out;
}

static std::string jstr(const json & j, const char * key)
{
    auto it = j.find(key);
    return (it != j.end() && it->is_string()) ? it->get<std::string>() : std::string();
}

/* Prefer synced lyrics, fall back to plain text. */
static bool from_json(const json & j, Lyrics & out)
{
    if(!j.is_object()) return false;
    std::string synced = jstr(j, "syncedLyrics");
    if(!synced.empty()) {
        Lyrics l = lyrics_parse_lrc(synced);
        if(!l.lines.empty()) {
            out = std::move(l);
            return true;
        }
    }
    std::string plain = jstr(j, "plainLyrics");
    if(!plain.empty()) {
        Lyrics l = lyrics_parse_plain(plain);
        if(!l.lines.empty()) {
            out = std::move(l);
            return true;
        }
    }
    return false;
}

bool lyrics_fetch(const MediaInfo & info, Lyrics & out)
{
    if(info.title.empty()) return false;
    const std::string base = "https://lrclib.net/api/";
    std::string q = "track_name=" + escape(info.title) + "&artist_name=" + escape(info.artists);

    /* Exact match first (needs album + duration), then a looser search. */
    std::string body;
    std::string get = base + "get?" + q;
    if(!info.album.empty()) get += "&album_name=" + escape(info.album);
    if(info.duration_sec > 0) get += "&duration=" + std::to_string(info.duration_sec);
    if(http_get_url(get, body) && from_json(json::parse(body, nullptr, false), out)) return true;

    body.clear();
    if(!http_get_url(base + "search?" + q, body)) return false;
    json arr = json::parse(body, nullptr, false);
    if(!arr.is_array()) return false;
    const json * best = nullptr;
    for(const auto & item : arr) {
        if(!item.is_object()) continue;
        auto d = item.find("duration");
        if(info.duration_sec > 0 && d != item.end() && d->is_number() &&
           std::abs(d->get<double>() - info.duration_sec) > 5.0)
            continue;
        if(!jstr(item, "syncedLyrics").empty()) { best = &item; break; }
        if(!best && !jstr(item, "plainLyrics").empty()) best = &item;
    }
    return best && from_json(*best, out);
}
