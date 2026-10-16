#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>
#include "fake_browse.h"
#include "music_ui.h"

namespace {

struct Track {
    const char * title;
    const char * artists;
};

struct Playlist {
    const char * name;
    std::vector<Track> tracks;
};

/* Mixed scripts on purpose: the rows must handle Hangul and kana, and long titles must be cut with "...". */
const std::vector<Playlist> PLAYLISTS = {
    {"Daily Mix 1",
     {{"Give Love", "AKMU"},
      {"소문의 낙원", "AKMU"},
      {"the NOISE (Contains a Samples of Into The Night)", "LE SSERAFIM"},
      {"Stop The Wedding!", "Sabrina Carpenter"},
      {"Patient Zero", "Chappell Roan"},
      {"あぶく", "Eve"},
      {"Lemon", "米津玄師"},
      {"Supernova", "aespa"},
      {"Magnetic", "ILLIT"},
      {"Hype Boy", "NewJeans"},
      {"Ditto", "NewJeans"},
      {"I AM", "IVE"}}},
    {"출근길 플레이리스트",
     {{"봄날", "BTS"},
      {"밤편지", "IU"},
      {"Love wins all", "IU"},
      {"첫눈처럼 너에게 가겠다", "에일리"},
      {"Spring Day", "BTS"},
      {"Palette (feat. G-DRAGON)", "IU"}}},
    {"作業用BGM",
     {{"夜に駆ける", "YOASOBI"},
      {"アイドル", "YOASOBI"},
      {"Pretender", "Official髭男dism"},
      {"うっせぇわ", "Ado"},
      {"KICK BACK", "米津玄師"}}},
    {"Liked Songs",
     {{"Espresso", "Sabrina Carpenter"},
      {"Birds of a Feather", "Billie Eilish"},
      {"Good Luck, Babe!", "Chappell Roan"},
      {"Please Please Please", "Sabrina Carpenter"}}},
    {"Empty playlist", {}},
};

/* What plays now: a playlist and a position in it. The queue is that playlist from there on, with a couple
 * of "added to queue" songs (no known context: view-only, like a Spotify queue entry) after the current one. */
int g_playlist = 0;
int g_index    = 0;
int g_open     = -1; /* playlist whose tracks the panel shows */

const Track EXTRA_QUEUED[] = {{"Dynamite", "BTS"}, {"Hello Future", "NCT DREAM"}};

std::string cover_key(const Track & t)
{
    return std::string(t.title) + "|" + t.artists;
}

/* Fake cover art: a diagonal two-color gradient with a disc, colors from a hash of the key. */
void make_thumb(const std::string & key, uint16_t * px)
{
    uint32_t h = 2166136261u;
    for(unsigned char c : key) h = (h ^ c) * 16777619u;
    auto rgb565 = [](int r, int g, int b) { return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)); };
    const int r1 = 60 + (h & 0xFF) % 160, g1 = 60 + ((h >> 8) & 0xFF) % 160, b1 = 60 + ((h >> 16) & 0xFF) % 160;
    const int r2 = 255 - r1, g2 = 255 - g1, b2 = 255 - b1;
    const int N = BROWSE_THUMB_PX;
    for(int y = 0; y < N; y++) {
        for(int x = 0; x < N; x++) {
            const int t = (x + y) * 255 / (2 * N - 2);
            int r = r1 + (r2 - r1) * t / 255, g = g1 + (g2 - g1) * t / 255, b = b1 + (b2 - b1) * t / 255;
            const int dx = x - N / 2, dy = y - N / 2;
            if(dx * dx + dy * dy < (N / 5) * (N / 5)) r = g = b = 235;
            px[y * N + x] = rgb565(r, g, b);
        }
    }
}

/* Answer a thumbnail request after a short delay, like a bridge fetching and resizing the cover would. */
void on_thumb_request(const char * key)
{
    auto * k = new std::string(key);
    lv_timer_t * t = lv_timer_create(
        [](lv_timer_t * tm) {
            auto * key = static_cast<std::string *>(lv_timer_get_user_data(tm));
            std::vector<uint16_t> px(BROWSE_THUMB_PX * BROWSE_THUMB_PX);
            make_thumb(*key, px.data());
            browse_ui_set_thumb(key->c_str(), px.data(), BROWSE_THUMB_PX, BROWSE_THUMB_PX);
            delete key;
        },
        120 + (uint32_t)(k->size() * 7 % 200), k);
    lv_timer_set_repeat_count(t, 1);
}

void push_queue()
{
    const Playlist & p = PLAYLISTS[g_playlist];
    std::vector<std::string> subs;
    std::vector<std::string> keys;
    std::vector<browse_item_t> items;
    subs.reserve(p.tracks.size() + 4);
    keys.reserve(p.tracks.size() + 4);

    auto add = [&](const Track & t, bool playable, bool current) {
        subs.push_back(t.artists);
        keys.push_back(cover_key(t));
        items.push_back({t.title, subs.back().c_str(), playable, current, keys.back().c_str()});
    };
    add(p.tracks[g_index], true, true);
    for(const Track & t : EXTRA_QUEUED) add(t, false, false);
    for(size_t i = g_index + 1; i < p.tracks.size(); i++) add(p.tracks[i], true, false);
    browse_ui_set_queue(items.data(), items.size());
}

void push_playlists()
{
    std::vector<std::string> subs;
    std::vector<std::string> keys;
    std::vector<browse_item_t> items;
    subs.reserve(PLAYLISTS.size());
    keys.reserve(PLAYLISTS.size());
    for(const Playlist & p : PLAYLISTS) {
        subs.push_back(std::to_string(p.tracks.size()) + (p.tracks.size() == 1 ? " track" : " tracks"));
        keys.push_back(std::string("playlist:") + p.name);
        items.push_back({p.name, subs.back().c_str(), true, false, keys.back().c_str()});
    }
    browse_ui_set_playlists(items.data(), items.size());
}

void push_tracks()
{
    const Playlist & p = PLAYLISTS[g_open];
    std::vector<std::string> keys;
    std::vector<browse_item_t> items;
    keys.reserve(p.tracks.size());
    for(size_t i = 0; i < p.tracks.size(); i++) {
        const bool now = (g_open == g_playlist && (int)i == g_index);
        keys.push_back(cover_key(p.tracks[i]));
        items.push_back({p.tracks[i].title, p.tracks[i].artists, true, now, keys.back().c_str()});
    }
    browse_ui_set_tracks(p.name, items.data(), items.size());
}

/* Queue rows: 0 = current, then the extras, then the rest of the playlist. */
int queue_row_to_index(int row)
{
    const int extras = (int)(sizeof(EXTRA_QUEUED) / sizeof(EXTRA_QUEUED[0]));
    return g_index + (row - extras); /* row 0 -> g_index, row extras+1 -> g_index+1 */
}

void on_browse(browse_evt_t evt, int index)
{
    switch(evt) {
        case BROWSE_EVT_SHOWN:
            push_queue();
            push_playlists();
            break;
        case BROWSE_EVT_OPEN_PLAYLIST:
            if(index < 0 || index >= (int)PLAYLISTS.size()) return;
            g_open = index;
            push_tracks();
            break;
        case BROWSE_EVT_PLAY_TRACK:
            if(g_open < 0 || index < 0 || index >= (int)PLAYLISTS[g_open].tracks.size()) return;
            g_playlist = g_open;
            g_index    = index;
            printf("[fake-browse] play \"%s\" from \"%s\"\n", PLAYLISTS[g_playlist].tracks[g_index].title, PLAYLISTS[g_playlist].name);
            push_tracks();
            push_queue();
            break;
        case BROWSE_EVT_PLAY_QUEUE: {
            const int i = (index == 0) ? g_index : queue_row_to_index(index);
            if(i < 0 || i >= (int)PLAYLISTS[g_playlist].tracks.size()) return;
            g_index = i;
            printf("[fake-browse] jump to \"%s\"\n", PLAYLISTS[g_playlist].tracks[g_index].title);
            push_queue();
            break;
        }
    }
}

} // namespace

void fake_browse_install(void)
{
    browse_ui_set_thumb_cb(on_thumb_request);
    music_ui_set_browse_cb(on_browse);
}
