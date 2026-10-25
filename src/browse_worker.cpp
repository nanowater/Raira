#include "browse_worker.h"
#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
#include "browse_ui.h"
#include "media_poller.h"
#include "music_ui.h"

namespace {

struct Req
{
    enum Kind { REFRESH, OPEN_PLAYLIST, PLAY_TRACK, PLAY_QUEUE, THUMB, ENDPOINT, TOKEN } kind = REFRESH;
    int index                                                                                 = 0;
    std::string text; /* cover key, or the new endpoint */
};

std::mutex g_mu;
std::condition_variable g_cv;
std::deque<Req> g_reqs;

void push(Req r)
{
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_reqs.push_back(std::move(r));
    }
    g_cv.notify_one();
}

/* LVGL thread: the panel asked for something. */
void on_browse(browse_evt_t evt, int index)
{
    Req r;
    r.index = index;
    switch(evt) {
        case BROWSE_EVT_SHOWN: r.kind = Req::REFRESH; break;
        case BROWSE_EVT_OPEN_PLAYLIST: r.kind = Req::OPEN_PLAYLIST; break;
        case BROWSE_EVT_PLAY_TRACK: r.kind = Req::PLAY_TRACK; break;
        case BROWSE_EVT_PLAY_QUEUE: r.kind = Req::PLAY_QUEUE; break;
    }
    push(std::move(r));
}

void on_thumb_request(const char * key)
{
    Req r;
    r.kind = Req::THUMB;
    r.text = key;
    push(std::move(r));
}

/* The panel copies what it is given, but the C structs only point into our strings: build them here, on the
 * LVGL thread, from items the worker handed over. */
std::vector<browse_item_t> to_ui(const std::vector<BrowseItem> & items)
{
    std::vector<browse_item_t> out;
    out.reserve(items.size());
    for(const BrowseItem & i : items)
        out.push_back(
            {i.title.c_str(), i.subtitle.c_str(), i.playable, i.current, i.cover.empty() ? nullptr : i.cover.c_str()});
    return out;
}

enum class Target { QUEUE, PLAYLISTS };

void post_list(Target t, std::vector<BrowseItem> items)
{
    auto shared = std::make_shared<std::vector<BrowseItem>>(std::move(items));
    media_post_to_ui([t, shared] {
        auto ui = to_ui(*shared);
        if(t == Target::QUEUE)
            browse_ui_set_queue(ui.data(), ui.size());
        else
            browse_ui_set_playlists(ui.data(), ui.size());
    });
}

/* RGB565 (little endian) from the decoder's BGRX pixels. */
std::vector<uint16_t> to_rgb565(const CoverPixels & px)
{
    std::vector<uint16_t> out((size_t)px.w * px.h);
    for(size_t i = 0; i < out.size(); i++) {
        const uint8_t * p = &px.data[i * 4]; /* B, G, R, X */
        out[i]            = (uint16_t)(((p[2] & 0xF8) << 8) | ((p[1] & 0xFC) << 3) | (p[0] >> 3));
    }
    return out;
}

void worker(std::unique_ptr<BrowseSource> src)
{
    std::vector<BrowseItem> queue, playlists, tracks;  /* what the panel shows: row index -> item */
    const std::chrono::milliseconds QUEUE_SETTLE(700); /* the service needs a moment to update its queue */

    auto refresh_queue = [&] {
        std::vector<BrowseItem> q;
        if(src->list_queue(q)) {
            queue = q;
            post_list(Target::QUEUE, std::move(q));
        }
    };

    while(true) {
        Req r;
        {
            std::unique_lock<std::mutex> lk(g_mu);
            g_cv.wait(lk, [] { return !g_reqs.empty(); });
            r = std::move(g_reqs.front());
            g_reqs.pop_front();
        }
        switch(r.kind) {
            case Req::REFRESH: {
                refresh_queue();
                std::vector<BrowseItem> p;
                if(src->list_playlists(p)) {
                    playlists = p;
                    post_list(Target::PLAYLISTS, std::move(p));
                }
                break;
            }
            case Req::OPEN_PLAYLIST: {
                if(r.index < 0 || r.index >= (int)playlists.size()) break;
                std::string title = playlists[r.index].title;
                std::vector<BrowseItem> t;
                if(!src->list_tracks(playlists[r.index].id, title, t)) break;
                tracks      = t;
                auto shared = std::make_shared<std::vector<BrowseItem>>(std::move(t));
                media_post_to_ui([title, shared] {
                    auto ui = to_ui(*shared);
                    browse_ui_set_tracks(title.c_str(), ui.data(), ui.size());
                });
                break;
            }
            case Req::PLAY_TRACK:
            case Req::PLAY_QUEUE: {
                const std::vector<BrowseItem> & list = r.kind == Req::PLAY_TRACK ? tracks : queue;
                if(r.index < 0 || r.index >= (int)list.size() || !list[r.index].playable) break;
                if(!src->play_item(list[r.index])) break;
                std::this_thread::sleep_for(QUEUE_SETTLE);
                refresh_queue();
                break;
            }
            case Req::THUMB: {
                CoverPixels px;
                if(!src->get_thumb(r.text, BROWSE_THUMB_PX, px) || px.w == 0 || px.h == 0) break;
                auto rgb         = std::make_shared<std::vector<uint16_t>>(to_rgb565(px));
                const uint32_t w = px.w, h = px.h;
                const std::string key = r.text;
                media_post_to_ui([key, rgb, w, h] { browse_ui_set_thumb(key.c_str(), rgb->data(), w, h); });
                break;
            }
            case Req::ENDPOINT: src->set_endpoint(r.text); break;
            case Req::TOKEN: src->set_token(r.text); break;
        }
    }
}

} // namespace

void browse_worker_start(std::unique_ptr<BrowseSource> source)
{
    if(!source) return;
    music_ui_set_browse_cb(on_browse);
    browse_ui_set_thumb_cb(on_thumb_request);
    std::thread(worker, std::move(source)).detach();
}

void browse_worker_set_endpoint(const std::string & spec)
{
    Req r;
    r.kind = Req::ENDPOINT;
    r.text = spec;
    push(std::move(r));
}

void browse_worker_set_token(const std::string & token)
{
    Req r;
    r.kind = Req::TOKEN;
    r.text = token;
    push(std::move(r));
}
