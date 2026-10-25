#include "lyrics.h"
#include <algorithm>
#include <chrono>
#include <thread>
#include "platform/http.h"
#include <nlohmann/json.hpp>

using nlohmann::json;

using CancelFn = std::function<bool()>;

/* LRCLIB answers 429 when requests come in bursts; back off and retry a couple of times. */
/* `lost` is set when there was no usable answer (network error, 5xx, 429 that did not clear); a 404 is an answer ("not
 * there"). */
static bool http_get_url(HttpClient & http, const std::string & url, std::string & body, const CancelFn & cancelled,
                         bool & lost)
{
    HttpRequest req;
    req.url               = url;
    req.user_agent        = "music-controller/0.1 (https://github.com/nanowater)";
    req.connect_timeout_s = 4;
    req.timeout_s         = 10;
    req.cancelled         = cancelled;

    for(int attempt = 0; attempt < 3; attempt++) {
        if(cancelled && cancelled()) return false;
        HttpResponse res = http.perform(req);
        if(res.ok() && !res.body.empty()) {
            body = std::move(res.body);
            return true;
        }
        if(res.status != 429) {
            lost = res.status != 404 && !(cancelled && cancelled());
            return false;
        }
        /* back off in short slices so a cancel is noticed quickly */
        for(int waited = 0; waited < 1500 * (attempt + 1); waited += 100) {
            if(cancelled && cancelled()) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
    lost = true;
    return false;
}

static std::string escape(const std::string & s)
{
    return url_escape(s);
}

static std::string trim(const std::string & s)
{
    size_t a = s.find_first_not_of(" \t\r\n");
    if(a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

/* LRCLIB text sometimes carries invisible direction/zero-width marks (e.g. U+200E). The font has no
 * glyph for them, so they would show up as boxes: drop them. */
static std::string strip_invisible(const std::string & s)
{
    std::string out;
    out.reserve(s.size());
    for(size_t i = 0; i < s.size();) {
        unsigned char c = (unsigned char)s[i];
        if(c == 0xE2 && i + 2 < s.size()) {
            unsigned char b = (unsigned char)s[i + 1], d = (unsigned char)s[i + 2];
            bool zero_width_or_dir =
                (b == 0x80 && ((d >= 0x8B && d <= 0x8F) || (d >= 0xAA && d <= 0xAE))) || /* U+200B-200F, 202A-202E */
                (b == 0x81 && d == 0xA0);                                                /* U+2060 */
            if(zero_width_or_dir) {
                i += 3;
                continue;
            }
        }
        if(c == 0xEF && i + 2 < s.size() && (unsigned char)s[i + 1] == 0xBB &&
           (unsigned char)s[i + 2] == 0xBF) { /* U+FEFF */
            i += 3;
            continue;
        }
        out += s[i++];
    }
    return out;
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
        std::string text = trim(strip_invisible(line.substr(i)));
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
        size_t nl = text.find('\n', pos);
        std::string line =
            trim(strip_invisible(text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos)));
        pos = nl == std::string::npos ? text.size() + 1 : nl + 1;
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
/* Synced lyrics rarely say when the last line ends, which left it highlighted until the track
 * finished (often through a long outro). Append an empty "gap" entry after it, using the typical
 * line spacing as the line's length, so the highlight clears once the singing stops. */
static void add_end_marker(Lyrics & l)
{
    if(l.lines.empty() || l.lines.back().text.empty()) return; /* the LRC already ends with a gap */
    std::vector<int> gaps;
    int prev = -1;
    for(const LyricLine & line : l.lines) {
        if(line.text.empty()) {
            prev = -1;
            continue;
        }
        if(prev >= 0 && line.time_ms > prev) gaps.push_back(line.time_ms - prev);
        prev = line.time_ms;
    }
    int len = 5000;
    if(!gaps.empty()) {
        std::nth_element(gaps.begin(), gaps.begin() + gaps.size() / 2, gaps.end());
        len = gaps[gaps.size() / 2];
    }
    len = std::min(std::max(len, 3000), 8000);
    l.lines.push_back({l.lines.back().time_ms + len, ""});
}

static bool from_json(const json & j, Lyrics & out, bool allow_synced = true)
{
    if(!j.is_object()) return false;
    std::string synced = allow_synced ? jstr(j, "syncedLyrics") : std::string();
    if(!synced.empty()) {
        Lyrics l = lyrics_parse_lrc(synced);
        bool has_text =
            std::any_of(l.lines.begin(), l.lines.end(), [](const LyricLine & x) { return !x.text.empty(); });
        if(has_text) {
            add_end_marker(l);
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
                uint32_t cp =
                    ((c & 0x0F) << 12) | (((unsigned char)s[i + 1] & 0x3F) << 6) | ((unsigned char)s[i + 2] & 0x3F);
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

bool lyrics_fetch(const MediaInfo & info, Lyrics & out, const std::function<bool()> & cancelled)
{
    if(info.title.empty()) return false;
    const std::string base = "https://lrclib.net/api/";
    auto http              = http_client_create();

    /* Candidates: the exact lookup (title/artist/album/duration must match closely) plus a
     * free-text search like the LRCLIB website, which also finds entries filed under another
     * title form (e.g. "あぶく - Bubble" for "Bubble"). We rank them ourselves. */
    std::vector<json> cands;
    std::vector<bool> is_exact;
    bool lost = false; /* a request gave no usable answer */

    std::string body;
    std::string get = base + "get?track_name=" + escape(info.title) + "&artist_name=" + escape(info.artists);
    if(!info.album.empty()) get += "&album_name=" + escape(info.album);
    if(info.duration_sec > 0) get += "&duration=" + std::to_string(info.duration_sec);
    if(http_get_url(*http, get, body, cancelled, lost)) {
        json j = json::parse(body, nullptr, false);
        if(j.is_object()) {
            cands.push_back(std::move(j));
            is_exact.push_back(true);
        }
    }

    body.clear();
    std::string q = strip_suffix(info.title) + " " + first_artist(info.artists);
    if(http_get_url(*http, base + "search?q=" + escape(q), body, cancelled, lost)) {
        json arr = json::parse(body, nullptr, false);
        if(arr.is_array()) {
            for(auto & item : arr) {
                if(!item.is_object()) continue;
                cands.push_back(std::move(item));
                is_exact.push_back(false);
            }
        }
    }

    if(cancelled && cancelled()) return false;
    /* "nothing found" is only believed when no request was lost: with one of them gone the candidates may simply be
     * missing */
    if(cands.empty() && lost) {
        out.failed = true;
        return false;
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
