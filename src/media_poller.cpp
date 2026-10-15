#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>
#include <lvgl/lvgl.h>
#include "cover_decoder.h"
#include "media_poller.h"
#include "music_ui.h"

namespace {

struct Update {
    MediaInfo info;
    CoverPixels cover;                /* empty data = unchanged */
    std::vector<unsigned char> png;   /* fallback for covers that are not JPEG */
};

/* Commands from the LVGL thread; drained by the poll thread so UI never blocks on HTTP. */
std::mutex g_mu;
std::condition_variable g_cv;
std::deque<music_ui_cmd_t> g_cmds;

void on_transport(music_ui_cmd_t cmd)
{
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if(g_cmds.size() < 8) g_cmds.push_back(cmd);
    }
    g_cv.notify_one();
}

void apply_update(void * p)
{
    Update * u = static_cast<Update *>(p);
    music_ui_state_t st{u->info.title.c_str(), u->info.artists.c_str(), u->info.current_sec, u->info.duration_sec,
                        u->info.status == "playing"};
    music_ui_update(&st);
    if(!u->cover.data.empty())
        music_ui_set_album_art_raw(u->cover.data.data(), u->cover.w, u->cover.h, LV_COLOR_FORMAT_XRGB8888);
    else if(!u->png.empty())
        music_ui_set_album_art_encoded(u->png.data(), u->png.size());
    delete u;
}

void poll_loop(TidalClient client)
{
    std::string cover_for; /* track_id the displayed cover belongs to */
    while(true) {
        /* Sleep until the next poll, or wake early for a transport command. */
        std::deque<music_ui_cmd_t> cmds;
        {
            std::unique_lock<std::mutex> lk(g_mu);
            g_cv.wait_for(lk, std::chrono::seconds(1), [] { return !g_cmds.empty(); });
            cmds.swap(g_cmds);
        }
        for(music_ui_cmd_t c : cmds) {
            switch(c) {
                case MUSIC_UI_CMD_PREVIOUS: client.previous(); break;
                case MUSIC_UI_CMD_PLAY_PAUSE: client.play_pause(); break;
                case MUSIC_UI_CMD_NEXT: client.next(); break;
            }
        }

        MediaInfo info;
        if(client.get_current(info)) {
            Update * u = new Update{info, {}, {}};
            if(!info.track_id.empty() && info.track_id != cover_for) {
                std::vector<unsigned char> img;
                if(client.get_cover(img)) {
                    /* tidal-hifi serves JPEG (labelled image/png); LVGL's JPEG decoder
                     * only reads files, so decode here, off the LVGL thread. */
                    if(cover_decode(img, MUSIC_UI_COVER_SIZE, u->cover))
                        cover_for = info.track_id;
                    else if(img.size() > 8 && img[0] == 0x89 && img[1] == 'P' && img[2] == 'N' && img[3] == 'G') {
                        u->png    = std::move(img);
                        cover_for = info.track_id;
                    }
                }
            }
            lv_async_call(apply_update, u);
        }
    }
}

} // namespace

void media_poller_start(const TidalConfig & cfg)
{
    music_ui_set_transport_cb(on_transport);
    std::thread(poll_loop, TidalClient(cfg)).detach();
}
