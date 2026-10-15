#include <lvgl/core/lv_area.h>
#include <string.h>
#include <cstdlib>
#include <thread>
#include <curl/curl.h>
#include <lvgl/lvgl.h>
#include "music_ui.h"

static lv_obj_t * cover = nullptr;

static size_t write_data(void * ptr, size_t size, size_t nmemb, void * stream)
{
    return fwrite(ptr, size, nmemb, (FILE *)stream);
}

static void apply_cover(void *)
{
    if(cover) {
        // LVGL supports the file if the matching decoder is enabled
        // (LodePNG for PNG by default, plus BMP).
        lv_image_set_src(cover, "A:/tmp/album.png");
    }
}

static void fetch_cover_thread()
{
    FILE * fp = fopen("/tmp/album.jpg", "wb");
    if(!fp) return;

    CURL * curl = curl_easy_init();
    if(curl) {
        curl_easy_setopt(curl, CURLOPT_URL, "http://localhost:47836/current/image");
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_data);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_perform(curl);
        curl_easy_cleanup(curl);
    }
    fclose(fp);

    // LVGL's image cache cannot hold a 1280x1280 raw buffer by default.
    // Resize to the cover widget size before asking LVGL to decode it.
    system("convert /tmp/album.jpg -resize 250x250 /tmp/album.png");

    lv_async_call(apply_cover, nullptr);
}

static void on_play_clicked(lv_event_t * e)
{
    lv_obj_t * btn   = lv_event_get_target_obj(e);
    lv_obj_t * label = lv_obj_get_child(btn, 0);
    if(label == nullptr) return;
    const char * cur = lv_label_get_text(label);
    lv_label_set_text(label, strcmp(cur, "Play") == 0 ? "Pause" : "Play");
}

void music_ui_create(void)
{
    lv_obj_t * screen = lv_screen_active();

    // 화면 배경
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x12151B), 0);

    // 커버 이미지 placeholder
    cover = lv_image_create(screen);
    lv_obj_set_size(cover, 250, 250);
    lv_obj_set_style_radius(cover, 16, LV_STATE_DEFAULT);
    lv_obj_set_style_clip_corner(cover, true, 0);
    lv_obj_align(cover, LV_ALIGN_DEFAULT, 30, 30);

    // 곡 제목 (커버 아래)
    lv_obj_t * title = lv_label_create(screen);
    lv_label_set_text(title, "Midnight City");
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align_to(title, cover, LV_ALIGN_OUT_BOTTOM_MID, 0, 12);

    // 재생 버튼
    lv_obj_t * play_button = lv_button_create(screen);
    lv_obj_set_size(play_button, 110, 64);
    lv_obj_align(play_button, LV_ALIGN_BOTTOM_MID, 0, -30);

    lv_obj_add_event_cb(play_button, on_play_clicked, LV_EVENT_CLICKED, nullptr);

    // 버튼 안에 텍스트 배치
    lv_obj_t * button_label = lv_label_create(play_button);
    lv_label_set_text(button_label, "Play");
    lv_obj_center(button_label);

    std::thread(fetch_cover_thread).detach();
}
