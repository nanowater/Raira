#include <chrono>
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

void apply_update(void * p)
{
    Update * u = static_cast<Update *>(p);
    music_ui_state_t st{u->info.title.c_str(), u->info.artists.c_str(), u->info.current_sec, u->info.duration_sec};
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
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

} // namespace

void media_poller_start(const TidalConfig & cfg)
{
    std::thread(poll_loop, TidalClient(cfg)).detach();
}
