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
#include "strings.h"

namespace {

/* The UI bitmask mirrors MediaCapability (media_source.h): these asserts keep the two lists in sync. */
static_assert(MUSIC_UI_CAP_SEEK == MEDIA_CAP_SEEK, "UI capability bits must match media_source.h");
static_assert(MUSIC_UI_CAP_SHUFFLE == MEDIA_CAP_SHUFFLE, "UI capability bits must match media_source.h");
static_assert(MUSIC_UI_CAP_REPEAT == MEDIA_CAP_REPEAT, "UI capability bits must match media_source.h");
static_assert(MUSIC_UI_CAP_PREV_NEXT == MEDIA_CAP_PREV_NEXT, "UI capability bits must match media_source.h");
static_assert(MUSIC_UI_CAP_BROWSE == MEDIA_CAP_BROWSE, "UI capability bits must match media_source.h");

/* Capabilities of the current service: set in media_poller_start, then updated by the poller thread
 * when a poll reports different ones (MediaInfo::caps). Guarded by g_mu once the threads run. */
uint32_t g_caps = 0;
LyricsProvider g_lyrics_provider; /* set in media_poller_start before the lyrics thread runs; empty = no lyrics */
media_browse_mode_t g_browse_mode = MEDIA_BROWSE_NONE;

/* The browse bit also depends on whether a browse provider exists at all. */
uint32_t with_browse_mode(uint32_t caps)
{
    if(g_browse_mode == MEDIA_BROWSE_NONE) return caps & ~(uint32_t)MEDIA_CAP_BROWSE;
    if(g_browse_mode == MEDIA_BROWSE_ALWAYS) return caps | MEDIA_CAP_BROWSE;
    return caps;
}

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

/* Last connection state handed to the UI (dedupe: post only on changes). */
std::atomic<bool> g_conn_posted{true};

void post_conn_state(bool connected)
{
    if(g_conn_posted.exchange(connected) == connected) return;
    post_to_ui([connected] {
        music_ui_set_connected(connected);
        if(!connected) music_ui_clear_content(); /* what is shown may be long out of date; the poller keeps the data */
    });
}

/* Everything derived from one cover, handed to the UI thread as one immutable piece. It is NOT kept after the UI took
 * it: the decoded cover (640 KB) and background (768 KB) would sit in PSRAM for good. The poller keeps only the
 * downloaded image (a few KB to 100 KB) of the last cover and decodes it again when the same artwork is due once more.
 */
struct CoverBundle
{
    CoverPixels cover;
    CoverPixels bg;                 /* full-screen ambient background for this cover, empty = none */
    std::vector<unsigned char> png; /* fallback for covers that are not JPEG */
};

struct Update
{
    MediaInfo info;
    std::shared_ptr<const CoverBundle> art; /* null = the cover is unchanged */
    bool clear_cover = false;               /* nothing is playing any more: drop the old cover */
};

/* Commands from the LVGL thread; drained by the poll thread so UI never blocks on HTTP. */
std::mutex g_mu;
std::condition_variable g_cv;
std::deque<music_ui_cmd_t> g_cmds;
/* Volume: steps update g_vol_target at once (the UI has nothing to show, but a fast turn must build
 * on the previous step, not on the stale polled value); the newest target is sent at most every 80 ms. */
const double VOLUME_STEP          = 0.02;
const int VOLUME_SEND_INTERVAL_MS = 80;
const int VOLUME_FOLLOW_MS        = 2000;  /* after the last step, trust the polled volume again */
double g_vol_polled               = -1;    /* last volume reported by /current */
double g_vol_target               = -1;    /* wanted volume, -1 = just follow the polled one */
bool g_vol_dirty                  = false; /* g_vol_target not sent yet */
std::chrono::steady_clock::time_point g_vol_last_send, g_vol_last_step;
std::string g_player; /* new player choice to apply on the poller thread */
bool g_player_dirty = false;
media_players_cb_t g_players_cb; /* pending list request: answered on the poller thread, delivered on the LVGL thread */
int g_seek_sec = -1;             /* latest requested seek position, -1 = none (only the newest matters) */

void volume_sync_polled(double polled)
{
    std::lock_guard<std::mutex> lk(g_mu);
    g_vol_polled = polled;
    if(g_vol_target >= 0 && !g_vol_dirty &&
       (std::abs(polled - g_vol_target) < 0.005 ||
        std::chrono::steady_clock::now() - g_vol_last_step > std::chrono::milliseconds(VOLUME_FOLLOW_MS)))
        g_vol_target = -1;
}

void set_player_pending(const std::string & id)
{
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_player       = id;
        g_player_dirty = true;
    }
    g_cv.notify_one();
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

struct LyricsRequest
{
    MediaInfo info;
    uint64_t gen = 0;
    std::string key; /* the metadata key the poller derived from `info` */
};
struct LyricsResult
{
    uint64_t gen = 0;
    bool found   = false;
    Lyrics lyrics;
};

const uint32_t SCREEN_W = 800, SCREEN_H = 480; /* the background is rendered at the panel size */
const int POLL_INTERVAL_MS = 500;              /* /current is ~1 KB and answers in <1 ms locally */
const int LYRICS_SETTLE_MS = 500;
/* A lookup that FAILED (not one that found nothing) is asked again while the same track plays: after these pauses, in
 * seconds. The PC already retries once itself, so these are long on purpose (LRCLIB rate-limits, and a lasting outage
 * must not be hammered). */
const int LYRICS_RETRY_AFTER_S[]      = {8, 30, 90};
const int CONNECTION_LOST_AFTER_FAILS = 3;

std::mutex g_lyr_mu;
std::condition_variable g_lyr_cv;
bool g_lyr_has_req = false;
LyricsRequest g_lyr_req;
std::atomic<uint64_t> g_lyr_gen{0}; /* newest requested track; older results are dropped */
/* The last lyrics that were found, with the key of their track: shown again without a lookup when the same track comes
 * back (after a reconnect, or when switching back to it). Only found lyrics are kept: a lookup that failed for a moment
 * must not turn into "no lyrics" for good. */
std::mutex g_lyr_cache_mu;
std::shared_ptr<LyricsResult> g_lyr_cache;
std::string g_lyr_cache_key;

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

        uint64_t gen   = req.gen;
        auto cancelled = [gen] { return g_lyr_gen.load() != gen; };
        for(size_t attempt = 0;; attempt++) {
            auto r   = std::make_shared<LyricsResult>();
            r->gen   = gen;
            r->found = g_lyrics_provider && g_lyrics_provider(req.info, r->lyrics, cancelled);
            if(cancelled()) break; /* another track: its request is pending, this answer is of no use */
            if(r->found) {
                std::lock_guard<std::mutex> lk(g_lyr_cache_mu);
                g_lyr_cache     = r;
                g_lyr_cache_key = req.key;
            }
            /* A lookup that FAILED is not an answer yet: while a retry is still to come the layout stays as it is (a
             * short note in the lyrics area), so a blip does not flip the screen to the big cover and back. "No lyrics"
             * (the big-cover layout) is shown when the track has none, or when the last retry failed too. */
            const bool will_retry = !r->found && r->lyrics.failed &&
                                    attempt < sizeof(LYRICS_RETRY_AFTER_S) / sizeof(LYRICS_RETRY_AFTER_S[0]);
            if(will_retry) {
                post_to_ui([gen] {
                    if(gen == g_lyr_gen.load()) music_ui_set_lyrics_message(str::LYRICS_RETRYING);
                });
            } else {
                post_to_ui([r] { apply_lyrics(r.get()); });
                break;
            }
            for(int waited = 0; waited < LYRICS_RETRY_AFTER_S[attempt] * 1000 && !cancelled(); waited += 100)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if(cancelled()) break;
        }
    }
}

/* The lyrics of `key` again, from the cache (null when the last found lyrics belong to another track). */
std::shared_ptr<LyricsResult> cached_lyrics(const std::string & key)
{
    std::lock_guard<std::mutex> lk(g_lyr_cache_mu);
    return (g_lyr_cache && g_lyr_cache_key == key) ? g_lyr_cache : nullptr;
}

void show_cached_lyrics(const std::shared_ptr<LyricsResult> & cached)
{
    auto r = std::make_shared<LyricsResult>(*cached);
    r->gen = ++g_lyr_gen; /* also drops whatever lookup of an older track may still be running */
    post_to_ui([r] { apply_lyrics(r.get()); });
}

void request_lyrics(const MediaInfo & info, const std::string & key)
{
    uint64_t gen = ++g_lyr_gen;
    post_to_ui([] { music_ui_set_lyrics_message(str::LOADING_LYRICS); });
    {
        std::lock_guard<std::mutex> lk(g_lyr_mu);
        g_lyr_req     = {info, gen, key};
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
    music_ui_state_t st{u->info.title.c_str(),
                        u->info.artists.c_str(),
                        u->info.current_sec,
                        u->info.duration_sec,
                        quality.c_str(),
                        quality_tier(u->info),
                        u->info.status == "playing",
                        u->info.shuffle,
                        u->info.repeat == "single" ? MUSIC_UI_REPEAT_SINGLE
                        : u->info.repeat == "all"  ? MUSIC_UI_REPEAT_ALL
                                                   : MUSIC_UI_REPEAT_OFF,
                        u->info.position_ms >= 0 ? (int)u->info.position_ms : -1};
    music_ui_update(&st);
    if(u->clear_cover) music_ui_clear_album_art();
    if(u->art) {
        const CoverBundle & a = *u->art;
        if(!a.cover.data.empty())
            music_ui_set_album_art_raw(a.cover.data.data(), a.cover.w, a.cover.h, LV_COLOR_FORMAT_XRGB8888);
        else if(!a.png.empty())
            music_ui_set_album_art_encoded(a.png.data(), a.png.size());
        if(!a.bg.data.empty()) music_ui_set_background(a.bg.data.data(), a.bg.w, a.bg.h);
    }
}

void poll_loop(std::unique_ptr<MediaSource> source)
{
    std::string cover_for;  /* cover key (image URL) the displayed cover belongs to */
    std::string lyrics_for; /* metadata key lyrics were requested for */
    auto last_poll    = std::chrono::steady_clock::now() - std::chrono::milliseconds(POLL_INTERVAL_MS);
    int failed_polls  = 0;     /* consecutive failed polls */
    bool lost_handled = false; /* the poller's own state was reset for the current loss of the connection */
    std::vector<unsigned char>
        art_cache_img; /* the last cover as downloaded (still encoded), and the key it belongs to */
    std::string art_cache_key;
    while(true) {
        /* Sleep until the next poll, or wake early for a transport command, a seek or a volume step. */
        std::deque<music_ui_cmd_t> cmds;
        int seek_sec    = -1;
        bool new_player = false;
        std::string player;
        media_players_cb_t list_cb;
        double volume = -1; /* >= 0: send this level */
        {
            using clk = std::chrono::steady_clock;
            std::unique_lock<std::mutex> lk(g_mu);
            g_cv.wait_until(lk, last_poll + std::chrono::milliseconds(POLL_INTERVAL_MS), [] {
                return !g_cmds.empty() || g_seek_sec >= 0 || g_vol_dirty || g_player_dirty || g_players_cb;
            });
            cmds.swap(g_cmds);
            seek_sec       = g_seek_sec;
            g_seek_sec     = -1;
            new_player     = g_player_dirty;
            player         = g_player;
            g_player_dirty = false;
            list_cb.swap(g_players_cb);
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
                case MUSIC_UI_CMD_PREVIOUS: source->previous(); break;
                case MUSIC_UI_CMD_PLAY_PAUSE: source->play_pause(); break;
                case MUSIC_UI_CMD_NEXT: source->next(); break;
                case MUSIC_UI_CMD_SHUFFLE: source->toggle_shuffle(); break;
                case MUSIC_UI_CMD_REPEAT: source->toggle_repeat(); break;
            }
        }

        if(new_player) source->set_player(player);
        if(list_cb) {
            auto players  = std::make_shared<std::vector<PlayerInfo>>();
            auto pick     = std::make_shared<std::string>();
            const bool ok = source->list_players(*players, *pick);
            post_to_ui([list_cb, ok, players, pick] { list_cb(ok, *players, *pick); });
        }
        if(seek_sec >= 0) source->seek_absolute(seek_sec);
        if(volume >= 0) source->set_volume(volume);

        /* Poll when due, or right after a command/seek (volume steps alone must not trigger a poll each). */
        const bool force = !cmds.empty() || seek_sec >= 0 || new_player;
        if(!force && std::chrono::steady_clock::now() < last_poll + std::chrono::milliseconds(POLL_INTERVAL_MS))
            continue;
        last_poll = std::chrono::steady_clock::now();

        MediaInfo info;
        const bool polled = source->get_current(info);
        /* Report only changes, and only after a few failures in a row so a single dropped poll
         * does not flash the banner; it disappears on the first successful poll. */
        if(polled) {
            failed_polls = 0;
            lost_handled = false;
            post_conn_state(true);
        } else if(++failed_polls >= CONNECTION_LOST_AFTER_FAILS) {
            post_conn_state(false);
            if(!lost_handled) {
                /* The screen is blanked (post_conn_state). Forget what it showed, so the first poll after the
                 * connection is back fills it again - from the cached cover and lyrics when the track is the same - and
                 * cancel a lyrics lookup that is still running: its answer must not appear on the blank screen. */
                lost_handled = true;
                cover_for.clear();
                lyrics_for.clear();
                ++g_lyr_gen;
            }
        }
        if(polled) {
            if(info.volume >= 0) volume_sync_polled(info.volume);
            if(info.caps >= 0) {
                const uint32_t caps = with_browse_mode((uint32_t)info.caps);
                bool changed;
                {
                    std::lock_guard<std::mutex> lk(g_mu);
                    changed = caps != g_caps;
                    g_caps  = caps;
                }
                if(changed) post_to_ui([caps] { music_ui_set_capabilities(caps); });
            }
            /* Key on the fields the lookup uses, not just the track URL: while skipping, tidal-hifi can
             * report the new URL before title/artist/duration catch up, and a lookup made from that
             * half-updated state would show the previous song's lyrics. */
            const std::string lyrics_key = info.track_id + '|' + info.title + '|' + info.artists + '|' + info.album +
                                           '|' + std::to_string(info.duration_sec);
            if(!info.title.empty() && lyrics_key != lyrics_for) {
                lyrics_for = lyrics_key;
                if(auto cached = cached_lyrics(lyrics_key))
                    show_cached_lyrics(cached);
                else
                    request_lyrics(info, lyrics_key);
            }
            auto u  = std::make_shared<Update>();
            u->info = info;
            /* Key the cover by its image URL: consecutive tracks of one album keep the cover,
             * and a new URL means new artwork is due. */
            const std::string cover_key = info.image_url.empty() ? info.track_id : info.image_url;
            if(!cover_key.empty() && cover_key != cover_for) {
                std::vector<unsigned char> img;
                bool got = false;
                if(art_cache_key == cover_key &&
                   !art_cache_img.empty()) { /* the same artwork again: reconnected, or switched back to it */
                    img = art_cache_img;
                    got = true;
                } else {
                    /* The source picks where the cover comes from and a size that is enough for the widget. */
                    got = source->get_cover(info, MUSIC_UI_COVER_SIZE, img);
                }
                if(got) {
                    auto bundle = std::make_shared<CoverBundle>();
                    /* JPEG is decoded here, off the LVGL thread (LVGL's JPEG decoder only reads files). */
                    bool ok = cover_decode(img, MUSIC_UI_COVER_SIZE, bundle->cover);
                    if(ok) cover_background(bundle->cover, SCREEN_W, SCREEN_H, MUSIC_UI_BG_BASE_COLOR, bundle->bg);
                    if(!ok && img.size() > 8 && img[0] == 0x89 && img[1] == 'P' && img[2] == 'N' && img[3] == 'G') {
                        bundle->png = img;
                        ok          = true;
                    }
                    if(ok) {
                        cover_for     = cover_key;
                        art_cache_key = cover_key;
                        art_cache_img = std::move(img);
                        u->art        = bundle;
                    }
                }
            }
            if(cover_key.empty() && !cover_for.empty()) { /* e.g. another player was picked and it has no track */
                cover_for.clear();
                u->clear_cover = true;
            }
            post_to_ui([u] { apply_update(u.get()); });
        }
    }
}

} // namespace

void media_post_to_ui(std::function<void()> fn)
{
    post_to_ui(std::move(fn));
}

void media_poller_start(std::unique_ptr<MediaSource> source, media_browse_mode_t browse)
{
    g_browse_mode     = browse;
    g_lyrics_provider = source->lyrics_provider();
    /* capabilities() runs on the creating thread here, before the source moves over to the poller. */
    g_caps = with_browse_mode(source->capabilities());
    music_ui_set_capabilities(g_caps); /* hide the controls this service cannot drive */
    music_ui_set_transport_cb(on_transport);
    music_ui_set_seek_cb(on_seek);
    lv_timer_create(drain_ui_queue, 20, nullptr); /* runs queued UI work on the LVGL thread */
    std::thread(lyrics_loop).detach();
    std::thread(poll_loop, std::move(source)).detach();
}

void media_poller_volume_step(int delta)
{
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if(!(g_caps & MEDIA_CAP_VOLUME)) return; /* the service has no volume to adjust */
        double base = g_vol_target >= 0 ? g_vol_target : g_vol_polled;
        if(base < 0 || delta == 0) return; /* volume not known yet */
        double v        = std::round((base + delta * VOLUME_STEP) * 100.0) / 100.0;
        g_vol_target    = v < 0 ? 0 : v > 1 ? 1 : v;
        g_vol_dirty     = true;
        g_vol_last_step = std::chrono::steady_clock::now();
    }
    g_cv.notify_one();
}

void media_poller_set_player(const std::string & id)
{
    set_player_pending(id);
}

void media_poller_list_players(media_players_cb_t done)
{
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_players_cb = std::move(done);
    }
    g_cv.notify_one();
}
