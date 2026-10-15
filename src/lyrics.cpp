#include "lyrics.h"
#include <algorithm>
#include <chrono>
#include <thread>
#include <curl/curl.h>
#include <nlohmann/json.hpp>

using nlohmann::json;

static size_t write_cb(void * ptr, size_t size, size_t nmemb, void * stream)
{
    static_cast<std::string *>(stream)->append(static_cast<char *>(ptr), size * nmemb);
    return size * nmemb;
}

static bool http_get_once(const std::string & url, std::string & body, long & code)
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
    code        = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    curl_easy_cleanup(curl);
    return rc == CURLE_OK && code == 200 && !body.empty();
}

/* LRCLIB answers 429 when requests come in bursts; back off and retry a couple of times. */
static bool http_get_url(const std::string & url, std::string & body)
{
    for(int attempt = 0; attempt < 3; attempt++) {
        long code = 0;
        body.clear();
        if(http_get_once(url, body, code)) return true;
        if(code != 429) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1500 * (attempt + 1)));
    }
    return false;
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
static bool from_json(const json & j, Lyrics & out, bool allow_synced = true)
{
    if(!j.is_object()) return false;
    std::string synced = allow_synced ? jstr(j, "syncedLyrics") : std::string();
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

/* Title without a trailing "(feat. ...)" / "[Remastered]" style suffix. */
static std::string strip_suffix(std::string t)
{
    for(const char * open : {" (", " ["}) {
        size_t p = t.rfind(open);
        if(p != std::string::npos && p > 0) t.erase(p);
    }
    return t;
}

static std::string first_artist(const std::string & artists)
{
    size_t c = artists.find(',');
    return trim(c == std::string::npos ? artists : artists.substr(0, c));
}

/* Share of kana/Hangul/Han among all Latin letters + those characters in the lyric text (UTF-8 scan).
 * Entries that merely add a translation line to English lyrics stay low; original-script lyrics are high. */
static double native_script_ratio(const Lyrics & l)
{
    size_t native = 0, latin = 0;
    for(const LyricLine & line : l.lines) {
        const std::string & s = line.text;
        for(size_t i = 0; i < s.size(); i++) {
            unsigned char c = (unsigned char)s[i];
            if(isalpha(c)) {
                latin++;
            } else if((c & 0xF0) == 0xE0 && i + 2 < s.size()) { /* 3-byte sequence: U+0800..U+FFFF */
                uint32_t cp = ((c & 0x0F) << 12) | (((unsigned char)s[i + 1] & 0x3F) << 6) | ((unsigned char)s[i + 2] & 0x3F);
                if((cp >= 0x3040 && cp <= 0x30FF) || (cp >= 0xAC00 && cp <= 0xD7A3) || (cp >= 0x4E00 && cp <= 0x9FFF))
                    native++;
                i += 2;
            }
        }
    }
    return (native + latin) ? (double)native / (double)(native + latin) : 0.0;
}

static std::string lower(std::string s)
{
    for(char & c : s) c = (char)tolower((unsigned char)c);
    return s;
}

/* Variants LRCLIB files next to the studio version; skip them unless the playing title has the same tag. */
static bool has_variant_tag(const std::string & title)
{
    std::string t = lower(title);
    for(const char * tag : {"remix", "live", "karaoke", "instrumental", "cover", "acoustic", "sped up", "slowed"})
        if(t.find(tag) != std::string::npos) return true;
    return false;
}

/* Timestamps running past the end of the track belong to a different edit (e.g. a 10 min remix). */
static bool synced_fits(const json & item, const MediaInfo & info)
{
    std::string synced = jstr(item, "syncedLyrics");
    if(synced.empty()) return false;
    if(info.duration_sec <= 0) return true;
    Lyrics l = lyrics_parse_lrc(synced);
    return !l.lines.empty() && l.lines.back().time_ms / 1000 <= info.duration_sec + 10;
}

/* Higher is better; negative means "do not use". `exact` marks the /get result. */
static int score_candidate(const json & item, size_t rank, bool exact, const MediaInfo & info)
{
    const std::string synced = jstr(item, "syncedLyrics");
    const std::string plain  = jstr(item, "plainLyrics");
    if(synced.empty() && plain.empty()) return -1;
    auto inst = item.find("instrumental");
    if(inst != item.end() && inst->is_boolean() && inst->get<bool>()) return -1;

    int score = 0;
    auto d    = item.find("duration");
    if(info.duration_sec > 0 && d != item.end() && d->is_number()) {
        double diff = std::abs(d->get<double>() - info.duration_sec);
        if(diff > 5.0) return -1;
        score += 100 - (int)(diff * 10); /* closer length = same recording */
    }

    bool use_synced = synced_fits(item, info);
    if(!use_synced && plain.empty()) return -1;
    if(use_synced) score += 100;

    if(has_variant_tag(jstr(item, "trackName")) && !has_variant_tag(info.title)) score -= 80;

    /* LRCLIB holds romanized and original-script versions of the same song; prefer the original
     * only when the text is mostly in that script (so English lyrics + a translation line don't win). */
    Lyrics text = use_synced ? lyrics_parse_lrc(synced) : lyrics_parse_plain(plain);
    if(native_script_ratio(text) >= 0.5) score += 60;
    if(!info.album.empty() && jstr(item, "albumName") == info.album) score += 20;
    if(exact) score += 30;
    score -= (int)std::min<size_t>(rank, 20); /* tie-break by LRCLIB's own ranking */
    return score;
}

bool lyrics_fetch(const MediaInfo & info, Lyrics & out)
{
    if(info.title.empty()) return false;
    const std::string base = "https://lrclib.net/api/";

    /* Candidates: the exact lookup (title/artist/album/duration must match closely) plus a
     * free-text search like the LRCLIB website, which also finds entries filed under another
     * title form (e.g. "あぶく - Bubble" for "Bubble"). We rank them ourselves. */
    std::vector<json> cands;
    std::vector<bool> is_exact;

    std::string body;
    std::string get = base + "get?track_name=" + escape(info.title) + "&artist_name=" + escape(info.artists);
    if(!info.album.empty()) get += "&album_name=" + escape(info.album);
    if(info.duration_sec > 0) get += "&duration=" + std::to_string(info.duration_sec);
    if(http_get_url(get, body)) {
        json j = json::parse(body, nullptr, false);
        if(j.is_object()) {
            cands.push_back(std::move(j));
            is_exact.push_back(true);
        }
    }

    body.clear();
    std::string q = strip_suffix(info.title) + " " + first_artist(info.artists);
    if(http_get_url(base + "search?q=" + escape(q), body)) {
        json arr = json::parse(body, nullptr, false);
        if(arr.is_array()) {
            for(auto & item : arr) {
                if(!item.is_object()) continue;
                cands.push_back(std::move(item));
                is_exact.push_back(false);
            }
        }
    }

    const json * best = nullptr;
    int best_score    = -1;
    for(size_t i = 0; i < cands.size(); i++) {
        int sc = score_candidate(cands[i], i, is_exact[i], info);
        if(sc > best_score) {
            best_score = sc;
            best       = &cands[i];
        }
    }
    return best && from_json(*best, out, synced_fits(*best, info));
}
