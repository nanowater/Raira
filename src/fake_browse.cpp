#include "fake_browse.h"
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace {

struct Track
{
    const char * title;
    const char * artists;
};

struct Playlist
{
    const char * name;
    std::vector<Track> tracks;
};

/* Mixed scripts on purpose: the rows must handle Hangul and kana, and long titles must be cut with "...". */
const std::vector<Playlist> PLAYLISTS = {
    {"Daily Mix 1",
     {{"Paper Lanterns", "The Example Band"},
      {"별빛 아래서", "Sample Artist"},
      {"The Quick Brown Fox Jumps Over the Lazy Dog (Extended Mix)", "Test Quartet"},
      {"Second Track", "Demo Singer"},
      {"Placeholder Song", "Mock Duo"},
      {"サンプルの歌", "Dummy Project"},
      {"仮の曲", "ダミー楽団"},
      {"Track Number Eight", "The Example Band"},
      {"Fake Song", "Demo Singer"},
      {"Lorem Ipsum", "Mock Duo"},
      {"Dolor Sit Amet", "Mock Duo"},
      {"Untitled", "Sample Artist"}}},
    {"출근길 플레이리스트",
     {{"봄날의 예시", "Sample Artist"},
      {"가짜 노래", "테스트 밴드"},
      {"Morning Commute", "Demo Singer"},
      {"첫 번째 곡", "더미 가수"},
      {"Spring Example", "Sample Artist"},
      {"Sample Song (feat. Guest Artist)", "Demo Singer"}}},
    {"作業用BGM",
     {{"夜のサンプル", "Dummy Project"},
      {"テスト曲", "Dummy Project"},
      {"ダミートラック", "ダミー楽団"},
      {"試験用の歌", "Test Quartet"},
      {"Background Loop", "Mock Duo"}}},
    {"Liked Songs",
     {{"Example One", "Demo Singer"},
      {"Example Two", "Test Quartet"},
      {"Example Three", "Sample Artist"},
      {"Example Four", "Demo Singer"}}},
    {"Empty playlist", {}},
};

const Track EXTRA_QUEUED[] = {{"Queued Example", "The Example Band"}, {"Another Queued Song", "Mock Duo"}};

std::string cover_key(const Track & t)
{
    return std::string(t.title) + "|" + t.artists;
}

/* Fake cover art: a diagonal two-color gradient with a disc, colors from a hash of the key. */
void make_thumb(const std::string & key, int n, CoverPixels & out)
{
    uint32_t h = 2166136261u;
    for(unsigned char c : key) h = (h ^ c) * 16777619u;
    const int r1 = 60 + (h & 0xFF) % 160, g1 = 60 + ((h >> 8) & 0xFF) % 160, b1 = 60 + ((h >> 16) & 0xFF) % 160;
    const int r2 = 255 - r1, g2 = 255 - g1, b2 = 255 - b1;
    out.w = out.h = (uint32_t)n;
    out.data.assign((size_t)n * n * 4, 0xFF);
    for(int y = 0; y < n; y++) {
        for(int x = 0; x < n; x++) {
            const int t = (x + y) * 255 / (2 * n - 2);
            int r = r1 + (r2 - r1) * t / 255, g = g1 + (g2 - g1) * t / 255, b = b1 + (b2 - b1) * t / 255;
            const int dx = x - n / 2, dy = y - n / 2;
            if(dx * dx + dy * dy < (n / 5) * (n / 5)) r = g = b = 235;
            uint8_t * p = &out.data[((size_t)y * n + x) * 4];
            p[0]        = (uint8_t)b; /* BGRX */
            p[1]        = (uint8_t)g;
            p[2]        = (uint8_t)r;
        }
    }
}

/* Contexts look like "fake:<playlist index>". */
std::string context_of(int playlist)
{
    return "fake:" + std::to_string(playlist);
}

int playlist_of(const std::string & context)
{
    return context.rfind("fake:", 0) == 0 ? atoi(context.c_str() + 5) : -1;
}

class FakeBrowse : public BrowseSource {
  public:
    bool always_available() const override
    {
        return true;
    }

    bool list_queue(std::vector<BrowseItem> & out) override
    {
        out.clear();
        const Playlist & p = PLAYLISTS[playlist_];
        out.push_back(item(playlist_, index_, true));
        for(const Track & t : EXTRA_QUEUED) { /* "added to queue" songs: no known context, so view-only */
            BrowseItem it;
            it.id       = t.title;
            it.title    = t.title;
            it.subtitle = t.artists;
            it.cover    = cover_key(t);
            out.push_back(std::move(it));
        }
        for(size_t i = index_ + 1; i < p.tracks.size(); i++) out.push_back(item(playlist_, (int)i, false));
        return true;
    }

    bool list_playlists(std::vector<BrowseItem> & out) override
    {
        out.clear();
        for(size_t i = 0; i < PLAYLISTS.size(); i++) {
            const Playlist & p = PLAYLISTS[i];
            BrowseItem it;
            it.id       = context_of((int)i);
            it.title    = p.name;
            it.subtitle = std::to_string(p.tracks.size()) + (p.tracks.size() == 1 ? " track" : " tracks");
            it.cover    = std::string("playlist:") + p.name;
            it.playable = true;
            out.push_back(std::move(it));
        }
        return true;
    }

    bool list_tracks(const std::string & playlist_id, std::string & playlist_title,
                     std::vector<BrowseItem> & out) override
    {
        const int pl = playlist_of(playlist_id);
        if(pl < 0 || pl >= (int)PLAYLISTS.size()) return false;
        playlist_title = PLAYLISTS[pl].name;
        out.clear();
        for(size_t i = 0; i < PLAYLISTS[pl].tracks.size(); i++)
            out.push_back(item(pl, (int)i, pl == playlist_ && (int)i == index_));
        return true;
    }

    bool play_item(const BrowseItem & it) override
    {
        const int pl = playlist_of(it.context);
        if(pl < 0 || pl >= (int)PLAYLISTS.size() || it.index < 0 || it.index >= (int)PLAYLISTS[pl].tracks.size())
            return false;
        playlist_ = pl;
        index_    = it.index;
        printf("[fake-browse] play \"%s\" from \"%s\"\n", PLAYLISTS[pl].tracks[it.index].title, PLAYLISTS[pl].name);
        fflush(stdout);
        return true;
    }

    bool get_thumb(const std::string & key, int px, CoverPixels & out) override
    {
        make_thumb(key, px, out);
        return true;
    }

  private:
    static BrowseItem item(int pl, int index, bool current)
    {
        const Track & t = PLAYLISTS[pl].tracks[index];
        BrowseItem it;
        it.id       = t.title;
        it.context  = context_of(pl);
        it.index    = index;
        it.title    = t.title;
        it.subtitle = t.artists;
        it.cover    = cover_key(t);
        it.playable = true;
        it.current  = current;
        return it;
    }

    int playlist_ = 0; /* what "plays now" */
    int index_    = 0;
};

} // namespace

std::unique_ptr<BrowseSource> fake_browse_create()
{
    return std::make_unique<FakeBrowse>();
}
