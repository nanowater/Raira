#include <atomic>
#include <cmath>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
#include "lvgl_compat.h"
#include "cover_decoder.h"
#include "lyrics.h"
#include "media_poller.h"
#include "music_ui.h"

namespace {

/* Background threads must not call LVGL (lv_async_call is not thread-safe: it creates a timer in
 * LVGL's global list). They queue work here instead; a timer on the LVGL thread runs it. */
std::mutex g_ui_mu;
std::deque<std::function<void()>> g_ui_queue;

void post_to_ui(std::function<void()> fn)
{
    std::lock_guard<std::mutex> lk(g_ui_mu);
    g_ui_queue.push_back(std::move(fn));
}

void drain_ui_queue(lv_timer_t *)
{
    std::deque<std::function<void()>> work;
    {
        std::lock_guard<std::mutex> lk(g_ui_mu);
        work.swap(g_ui_queue);
    }
    for(auto & fn : work) fn();
}

struct Update {
    MediaInfo info;
    CoverPixels cover;                /* empty data = unchanged */
    CoverPixels bg;                   /* full-screen ambient background for this cover, empty = none */
    std::vector<unsigned char> png;   /* fallback for covers that are not JPEG */
};

/* Commands from the LVGL thread; drained by the poll thread so UI never blocks on HTTP. */
std::mutex g_mu;
std::condition_variable g_cv;
std::deque<music_ui_cmd_t> g_cmds;
/* Volume: steps update g_vol_target at once (the UI has nothing to show, but a fast turn must build
 * on the previous step, not on the stale polled value); the newest target is sent at most every 80 ms. */
const double VOLUME_STEP         = 0.02;
const int VOLUME_SEND_INTERVAL_MS = 80;
const int VOLUME_FOLLOW_MS        = 2000; /* after the last step, trust the polled volume again */
double g_vol_polled = -1;                  /* last volume reported by /current */
double g_vol_target = -1;                  /* wanted volume, -1 = just follow the polled one */
bool g_vol_dirty    = false;               /* g_vol_target not sent yet */
std::chrono::steady_clock::time_point g_vol_last_send, g_vol_last_step;
int g_seek_sec = -1; /* latest requested seek position, -1 = none (only the newest matters) */

void volume_sync_polled(double polled)
{
    std::lock_guard<std::mutex> lk(g_mu);
    g_vol_polled = polled;
    if(g_vol_target >= 0 && !g_vol_dirty &&
       (std::abs(polled - g_vol_target) < 0.005 ||
        std::chrono::steady_clock::now() - g_vol_last_step > std::chrono::milliseconds(VOLUME_FOLLOW_MS)))
        g_vol_target = -1;
}

void on_seek(int seconds)
{
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_seek_sec = seconds;
    }
    g_cv.notify_one();
}

void on_transport(music_ui_cmd_t cmd)
{
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if(g_cmds.size() < 8) g_cmds.push_back(cmd);
    }
    g_cv.notify_one();
}

/* ---- Lyrics: fetched on their own thread so a slow LRCLIB never stalls polling ---- */

struct LyricsRequest {
    MediaInfo info;
    uint64_t gen = 0;
};
struct LyricsResult {
    uint64_t gen = 0;
    bool found   = false;
    Lyrics lyrics;
};

const uint32_t SCREEN_W = 800, SCREEN_H = 480; /* the background is rendered at the panel size */
const int POLL_INTERVAL_MS = 500; /* /current is ~1 KB and answers in <1 ms locally */
const int LYRICS_SETTLE_MS = 500;
const int CONNECTION_LOST_AFTER_FAILS = 3;

std::mutex g_lyr_mu;
std::condition_variable g_lyr_cv;
bool g_lyr_has_req = false;
LyricsRequest g_lyr_req;
std::atomic<uint64_t> g_lyr_gen{0}; /* newest requested track; older results are dropped */

void apply_lyrics(const LyricsResult * r)
{
    if(r->gen == g_lyr_gen.load()) {
        if(r->found) {
            std::vector<music_ui_lyric_line_t> lines;
            lines.reserve(r->lyrics.lines.size());
            for(const LyricLine & l : r->lyrics.lines) lines.push_back({l.text.c_str(), l.time_ms});
            music_ui_set_lyrics(lines.data(), lines.size(), r->lyrics.synced);
        } else {
            music_ui_set_no_lyrics(); /* switches to the layout without the lyrics panel */
        }
    }
}

void lyrics_loop()
{
    while(true) {
        LyricsRequest req;
        {
            std::unique_lock<std::mutex> lk(g_lyr_mu);
            g_lyr_cv.wait(lk, [] { return g_lyr_has_req; });
            req           = std::move(g_lyr_req);
            g_lyr_has_req = false;
        }
        /* Debounce: only look lyrics up once the track has stayed current for LYRICS_SETTLE_MS, so
         * skipping through songs doesn't fire a request for every one of them. */
        bool superseded = false;
        for(int waited = 0; waited < LYRICS_SETTLE_MS; waited += 50) {
            if(g_lyr_gen.load() != req.gen) {
                superseded = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        if(superseded) continue; /* the newer request is already pending */

        auto r  = std::make_shared<LyricsResult>();
        r->gen  = req.gen;
        uint64_t gen     = req.gen;
        r->found         = lyrics_fetch(req.info, r->lyrics, [gen] { return g_lyr_gen.load() != gen; });
        post_to_ui([r] { apply_lyrics(r.get()); });
    }
}

void request_lyrics(const MediaInfo & info)
{
    uint64_t gen = ++g_lyr_gen;
    post_to_ui([] { music_ui_set_lyrics_message("가사를 불러오는 중..."); });
    {
        std::lock_guard<std::mutex> lk(g_lyr_mu);
        g_lyr_req     = {info, gen};
        g_lyr_has_req = true;
    }
    g_lyr_cv.notify_one();
}

/* Text for the quality badge: the quality tier by name ("Hi-Res Lossless"); the service's own badge
 * text ("24-bit 48.0kHz") only when it reports no tier. Unknown tiers are shown as sent. */
std::string quality_text(const MediaInfo & m)
{
    if(m.quality == "HI_RES_LOSSLESS") return "Hi-Res Lossless";
    if(m.quality == "HI_RES") return "Hi-Res";
    if(m.quality == "LOSSLESS") return "Lossless";
    if(m.quality == "HIGH") return "High";
    if(m.quality == "LOW") return "Low";
    return m.quality.empty() ? m.quality_badge : m.quality;
}

int quality_tier(const MediaInfo & m)
{
    if(m.quality == "HI_RES_LOSSLESS" || m.quality == "HI_RES") return MUSIC_UI_QUALITY_HIRES;
    if(m.quality == "LOSSLESS") return MUSIC_UI_QUALITY_LOSSLESS;
    return MUSIC_UI_QUALITY_NORMAL;
}

void apply_update(const Update * u)
{
    const std::string quality = quality_text(u->info);
    music_ui_state_t st{u->info.title.c_str(), u->info.artists.c_str(), u->info.current_sec, u->info.duration_sec,
                        quality.c_str(), quality_tier(u->info), u->info.status == "playing", u->info.shuffle,
                        u->info.repeat == "single" ? MUSIC_UI_REPEAT_SINGLE
                        : u->info.repeat == "all"  ? MUSIC_UI_REPEAT_ALL
                                                   : MUSIC_UI_REPEAT_OFF};
    music_ui_update(&st);
    if(!u->cover.data.empty())
        music_ui_set_album_art_raw(u->cover.data.data(), u->cover.w, u->cover.h, LV_COLOR_FORMAT_XRGB8888);
    else if(!u->png.empty())
        music_ui_set_album_art_encoded(u->png.data(), u->png.size());
    if(!u->bg.data.empty()) music_ui_set_background(u->bg.data.data(), u->bg.w, u->bg.h);
}

void poll_loop(TidalClient client)
{
    std::string cover_for;  /* cover key (image URL) the displayed cover belongs to */
    std::string lyrics_for; /* metadata key lyrics were requested for */
    auto last_poll = std::chrono::steady_clock::now() - std::chrono::milliseconds(POLL_INTERVAL_MS);
    int failed_polls = 0;
    bool connected   = true; /* the banner stays hidden until polls actually fail */
    while(true) {
        /* Sleep until the next poll, or wake early for a transport command, a seek or a volume step. */
        std::deque<music_ui_cmd_t> cmds;
        int seek_sec     = -1;
        double volume    = -1; /* >= 0: send this level */
        {
            using clk = std::chrono::steady_clock;
            std::unique_lock<std::mutex> lk(g_mu);
            g_cv.wait_until(lk, last_poll + std::chrono::milliseconds(POLL_INTERVAL_MS),
                            [] { return !g_cmds.empty() || g_seek_sec >= 0 || g_vol_dirty; });
            cmds.swap(g_cmds);
            seek_sec   = g_seek_sec;
            g_seek_sec = -1;
            if(g_vol_dirty) {
                auto ready = g_vol_last_send + std::chrono::milliseconds(VOLUME_SEND_INTERVAL_MS);
                if(clk::now() < ready) {
                    lk.unlock(); /* too soon after the previous request: let more steps pile up first */
                    std::this_thread::sleep_until(ready);
                    lk.lock();
                }
                volume          = g_vol_target;
                g_vol_dirty     = false;
                g_vol_last_send = clk::now();
            }
        }
        for(music_ui_cmd_t c : cmds) {
            switch(c) {
                case MUSIC_UI_CMD_PREVIOUS: client.previous(); break;
                case MUSIC_UI_CMD_PLAY_PAUSE: client.play_pause(); break;
                case MUSIC_UI_CMD_NEXT: client.next(); break;
                case MUSIC_UI_CMD_SHUFFLE: client.toggle_shuffle(); break;
                case MUSIC_UI_CMD_REPEAT: client.toggle_repeat(); break;
            }
        }

        if(seek_sec >= 0) client.seek_absolute(seek_sec);
        if(volume >= 0) client.set_volume(volume);

        /* Poll when due, or right after a command/seek (volume steps alone must not trigger a poll each). */
        const bool force = !cmds.empty() || seek_sec >= 0;
        if(!force && std::chrono::steady_clock::now() < last_poll + std::chrono::milliseconds(POLL_INTERVAL_MS)) continue;
        last_poll = std::chrono::steady_clock::now();

        MediaInfo info;
        const bool polled = client.get_current(info);
        /* Report only changes, and only after a few failures in a row so a single dropped poll
         * does not flash the banner; it disappears on the first successful poll. */
        if(polled) {
            failed_polls = 0;
            if(!connected) {
                connected = true;
                post_to_ui([] { music_ui_set_connected(true); });
            }
        } else if(++failed_polls >= CONNECTION_LOST_AFTER_FAILS && connected) {
            connected = false;
            post_to_ui([] { music_ui_set_connected(false); });
        }
        if(polled) {
            if(info.volume >= 0) volume_sync_polled(info.volume);
            /* Key on the fields the lookup uses, not just the track URL: while skipping, tidal-hifi can
             * report the new URL before title/artist/duration catch up, and a lookup made from that
             * half-updated state would show the previous song's lyrics. */
            const std::string lyrics_key = info.track_id + '|' + info.title + '|' + info.artists + '|' + info.album + '|' +
                                           std::to_string(info.duration_sec);
            if(!info.title.empty() && lyrics_key != lyrics_for) {
                lyrics_for = lyrics_key;
                request_lyrics(info);
            }
            auto u = std::make_shared<Update>(Update{info, {}, {}, {}});
            /* Key the cover by its image URL: consecutive tracks of one album keep the cover,
             * and a new URL means new artwork is due. */
            const std::string cover_key = info.image_url.empty() ? info.track_id : info.image_url;
            if(!cover_key.empty() && cover_key != cover_for) {
                std::vector<unsigned char> img;
                /* Fetch from the artwork URL itself (a smaller size is plenty for the widget).
                 * tidal-hifi's /current/image serves a file it rewrites after the metadata changes, so
                 * while skipping tracks quickly it can lag behind or return an in-between track's art;
                 * it is only the fallback when the CDN is unreachable. */
                bool got = false;
                if(!info.image_url.empty()) {
                    std::string small = info.image_url;
                    size_t p          = small.find("1280x1280");
                    if(p != std::string::npos) small.replace(p, 9, "640x640");
                    got = client.get_cover_from_url(small, img);
                }
                if(!got) got = client.get_cover(img);
                if(got) {
                    /* JPEG is decoded here, off the LVGL thread (LVGL's JPEG decoder only reads files). */
                    bool ok = cover_decode(img, MUSIC_UI_COVER_SIZE, u->cover);
                    if(ok) cover_background(u->cover, SCREEN_W, SCREEN_H, MUSIC_UI_BG_BASE_COLOR, u->bg);
                    if(!ok && img.size() > 8 && img[0] == 0x89 && img[1] == 'P' && img[2] == 'N' && img[3] == 'G') {
                        u->png = std::move(img);
                        ok     = true;
                    }
                    if(ok) cover_for = cover_key;
                }
            }
            post_to_ui([u] { apply_update(u.get()); });
        }
    }
}

} // namespace

void media_poller_start(const TidalConfig & cfg)
{
    music_ui_set_transport_cb(on_transport);
    music_ui_set_seek_cb(on_seek);
    lv_timer_create(drain_ui_queue, 20, nullptr); /* runs queued UI work on the LVGL thread */
    std::thread(lyrics_loop).detach();
    std::thread(poll_loop, TidalClient(cfg)).detach();
}

void media_poller_volume_step(int delta)
{
    {
        std::lock_guard<std::mutex> lk(g_mu);
        double base = g_vol_target >= 0 ? g_vol_target : g_vol_polled;
        if(base < 0 || delta == 0) return; /* volume not known yet */
        double v = std::round((base + delta * VOLUME_STEP) * 100.0) / 100.0;
        g_vol_target    = v < 0 ? 0 : v > 1 ? 1 : v;
        g_vol_dirty     = true;
        g_vol_last_step = std::chrono::steady_clock::now();
    }
    g_cv.notify_one();
}
