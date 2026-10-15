#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <lvgl/lvgl.h>
#include <src/misc/cache/instance/lv_image_cache.h>
#include "music_ui.h"

extern "C" const lv_font_t font_mono_13; /* DejaVu Sans Mono 13px, digits and ":" only (src/fonts) */
extern "C" const unsigned char * noto_sans_kr_get(size_t * size);      /* embedded Noto Sans KR subset */
extern "C" const unsigned char * noto_sans_kr_bold_get(size_t * size); /* same glyph set, Bold */

/* Noto Sans KR at the given px size via TinyTTF; falls back to `fallback` if loading fails. */
static const lv_font_t * noto_font(int32_t px, const lv_font_t * fallback, bool bold = false)
{
    size_t size                = 0;
    const unsigned char * data = bold ? noto_sans_kr_bold_get(&size) : noto_sans_kr_get(&size);
    lv_font_t * f              = lv_tiny_ttf_create_data(data, size, px);
    if(!f) return fallback;
    f->fallback = fallback; /* glyphs outside the subset (e.g. symbols) */
    return f;
}

static lv_obj_t * cover                     = nullptr;
static lv_obj_t * title_box                 = nullptr;
static lv_obj_t * title                     = nullptr;
static lv_obj_t * artists                   = nullptr;
static lv_obj_t * progress_panel            = nullptr;
static lv_obj_t * progress_bar              = nullptr;
static lv_obj_t * current_time_lbl          = nullptr;
static lv_obj_t * duration_time_lbl         = nullptr;
static lv_timer_t * title_scroll_timer      = nullptr;
static lv_obj_t * play_label                = nullptr;
static music_ui_transport_cb_t transport_cb = nullptr;

/* After a click the icon flips right away; polled status is ignored until it agrees or the
 * grace period ends (tidal-hifi reports the old state for a moment). */
static const uint32_t OPTIMISTIC_GRACE_MS = 2000;
static const uint32_t ICON_ON             = 0xFFFFFF;
static const uint32_t ICON_OFF            = 0x6B7280;

struct Optimistic {
    bool active    = false;
    uint32_t since = 0;
    void start()
    {
        active = true;
        since  = lv_tick_get();
    }
    /* Returns true while the polled value should be ignored. */
    bool holds(bool polled_matches_shown)
    {
        if(active && (polled_matches_shown || lv_tick_elaps(since) > OPTIMISTIC_GRACE_MS)) active = false;
        return active;
    }
};

static bool shown_playing  = false;
static bool shown_shuffle  = false;
static int shown_repeat    = MUSIC_UI_REPEAT_OFF;
static Optimistic opt_play, opt_shuffle, opt_repeat;
static lv_obj_t * shuffle_label    = nullptr;
static lv_obj_t * repeat_label     = nullptr;
static lv_obj_t * repeat_one_label = nullptr; /* small "1" over the loop icon */

static void show_playing(bool playing)
{
    shown_playing = playing;
    if(play_label) lv_label_set_text(play_label, playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
}

static void show_shuffle(bool on)
{
    shown_shuffle = on;
    if(shuffle_label) lv_obj_set_style_text_color(shuffle_label, lv_color_hex(on ? ICON_ON : ICON_OFF), 0);
}

static void show_repeat(int mode)
{
    shown_repeat = mode;
    if(repeat_label) lv_obj_set_style_text_color(repeat_label, lv_color_hex(mode != MUSIC_UI_REPEAT_OFF ? ICON_ON : ICON_OFF), 0);
    if(repeat_one_label) lv_obj_set_hidden(repeat_one_label, mode != MUSIC_UI_REPEAT_SINGLE);
}

/* Album art buffer (encoded PNG or decoded pixels) owned by the UI. */
static unsigned char * art_data = nullptr;
static lv_image_dsc_t art_dsc;

static void fmt_time(int sec, char * buf, size_t n)
{
    if(sec < 0) sec = 0;
    snprintf(buf, n, "%02d:%02d", sec / 60, sec % 60);
}

static void enable_title_scroll_cb(lv_timer_t * t)
{
    if(title) lv_label_set_long_mode(title, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
    if(t) lv_timer_del(t);
    title_scroll_timer = nullptr;
}

static void set_label_if_changed(lv_obj_t * lbl, const char * text)
{
    if(lbl && strcmp(lv_label_get_text(lbl), text) != 0) lv_label_set_text(lbl, text);
}

/* ---- Lyrics ------------------------------------------------------------ */

static lv_obj_t * lyrics_panel       = nullptr;
static const lv_font_t * lyrics_font = nullptr;
static std::vector<lv_obj_t *> lyric_labels;
static std::vector<int> lyric_times;
static bool lyrics_synced = false;
static int lyric_cur      = -1;

/* Playback position, interpolated between the 1 s polls so line changes feel on time. */
static bool pos_playing    = false;
static int32_t pos_base_ms = 0;
static uint32_t pos_tick   = 0;

static const uint32_t LYRIC_COLOR_DIM = 0x6B7280;
static const int32_t LYRICS_ANCHOR_Y  = 130; /* px from the panel top where the current line rests */

static int32_t position_ms()
{
    return pos_base_ms + (pos_playing ? (int32_t)lv_tick_elaps(pos_tick) : 0);
}

static void lyrics_clear_labels()
{
    if(lyrics_panel) lv_obj_clean(lyrics_panel);
    lyric_labels.clear();
    lyric_times.clear();
    lyric_cur = -1;
}

static void lyrics_highlight(int idx)
{
    if(idx == lyric_cur) return;
    if(lyric_cur >= 0 && lyric_cur < (int)lyric_labels.size())
        lv_obj_set_style_text_color(lyric_labels[lyric_cur], lv_color_hex(LYRIC_COLOR_DIM), 0);
    lyric_cur = idx;
    if(idx < 0 || idx >= (int)lyric_labels.size()) return;

    lv_obj_t * l = lyric_labels[idx];
    lv_obj_set_style_text_color(l, lv_color_white(), 0);
    lv_obj_update_layout(lyrics_panel);
    /* lv_obj_get_y() is relative to the content area (below the top padding), so scrolling by it
     * leaves the current line at LYRICS_ANCHOR_Y from the panel top. */
    lv_obj_scroll_to_y(lyrics_panel, lv_obj_get_y(l), LV_ANIM_ON);
}

static void lyrics_tick_cb(lv_timer_t *)
{
    if(!lyrics_synced || lyric_times.empty()) return;
    int32_t pos = position_ms();
    int idx     = -1;
    for(int i = 0; i < (int)lyric_times.size(); i++) {
        if(lyric_times[i] <= pos)
            idx = i;
        else
            break;
    }
    lyrics_highlight(idx);
}

void music_ui_set_lyrics_message(const char * msg)
{
    if(!lyrics_panel) return;
    lyrics_synced = false;
    lyrics_clear_labels();
    lv_obj_t * l = lv_label_create(lyrics_panel);
    lv_label_set_text(l, msg ? msg : "");
    lv_obj_set_style_text_font(l, lyrics_font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(LYRIC_COLOR_DIM), 0);
}

void music_ui_set_lyrics(const music_ui_lyric_line_t * lines, size_t count, bool synced)
{
    if(!lyrics_panel) return;
    lyrics_clear_labels();
    lyrics_synced = synced && count > 0;
    lv_obj_scroll_to_y(lyrics_panel, 0, LV_ANIM_OFF);

    for(size_t i = 0; i < count; i++) {
        lv_obj_t * l = lv_label_create(lyrics_panel);
        lv_obj_set_width(l, lv_pct(100));
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
        lv_label_set_text(l, lines[i].text ? lines[i].text : "");
        lv_obj_set_style_text_font(l, lyrics_font, 0);
        lv_obj_set_style_text_color(l, lv_color_hex(synced ? LYRIC_COLOR_DIM : 0xD1D5DB), 0);
        lyric_labels.push_back(l);
        lyric_times.push_back(lines[i].time_ms);
    }
    if(lyrics_synced) lyrics_tick_cb(nullptr);
}

static void lyrics_sync_position(const music_ui_state_t * st)
{
    int32_t reported = st->current_sec * 1000 + 500; /* polls report whole seconds */
    bool keep        = pos_playing && st->playing && std::abs(position_ms() - reported) <= 1000;
    pos_playing      = st->playing;
    if(!keep) {
        pos_base_ms = reported;
        pos_tick    = lv_tick_get();
    }
}

void music_ui_set_transport_cb(music_ui_transport_cb_t cb)
{
    transport_cb = cb;
}

static void on_transport_clicked(lv_event_t * e)
{
    music_ui_cmd_t cmd = (music_ui_cmd_t)(intptr_t)lv_event_get_user_data(e);
    if(cmd == MUSIC_UI_CMD_PLAY_PAUSE) {
        show_playing(!shown_playing);
        opt_play.start();
    } else if(cmd == MUSIC_UI_CMD_SHUFFLE) {
        show_shuffle(!shown_shuffle);
        opt_shuffle.start();
    } else if(cmd == MUSIC_UI_CMD_REPEAT) {
        /* tidal-hifi cycles off -> all -> single (enum order) */
        show_repeat((shown_repeat + 1) % 3);
        opt_repeat.start();
    }
    if(transport_cb) transport_cb(cmd);
}

/* Round icon button using a built-in LVGL symbol. Returns the label (for icon swaps). */
static lv_obj_t * create_transport_button(lv_obj_t * parent, const char * symbol, int32_t size, const lv_font_t * font,
                                          music_ui_cmd_t cmd)
{
    lv_obj_t * btn = lv_button_create(parent);
    lv_obj_set_size(btn, size, size);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_20, LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(btn, lv_color_white(), LV_STATE_PRESSED);
    lv_obj_add_event_cb(btn, on_transport_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)cmd);

    lv_obj_t * lbl = lv_label_create(btn);
    lv_label_set_text(lbl, symbol);
    lv_obj_set_style_text_font(lbl, font, 0);
    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
    lv_obj_center(lbl);
    return lbl;
}

void music_ui_update(const music_ui_state_t * st)
{
    if(!st) return;
    lyrics_sync_position(st);
    if(!opt_play.holds(st->playing == shown_playing)) show_playing(st->playing);
    if(!opt_shuffle.holds(st->shuffle == shown_shuffle)) show_shuffle(st->shuffle);
    if(!opt_repeat.holds(st->repeat == shown_repeat)) show_repeat(st->repeat);
    const char * new_title   = st->title ? st->title : "";
    const char * new_artists = st->artists ? st->artists : "";

    if(title && strcmp(lv_label_get_text(title), new_title) != 0) {
        /* New track: restart from a clipped label, then enable scrolling after a delay. */
        lv_label_set_long_mode(title, LV_LABEL_LONG_MODE_CLIP);
        lv_label_set_text(title, new_title);
        if(title_scroll_timer) lv_timer_del(title_scroll_timer);
        title_scroll_timer = lv_timer_create(enable_title_scroll_cb, 2000, nullptr);
    }
    set_label_if_changed(artists, new_artists);

    if(progress_bar) {
        int v = 0;
        if(st->duration_sec > 0) {
            v = (int)(((int64_t)st->current_sec * 1000) / st->duration_sec);
            if(v < 0) v = 0;
            if(v > 1000) v = 1000;
        }
        lv_bar_set_value(progress_bar, v, LV_ANIM_OFF);
    }

    char buf[16];
    fmt_time(st->current_sec, buf, sizeof(buf));
    set_label_if_changed(current_time_lbl, buf);
    fmt_time(st->duration_sec, buf, sizeof(buf));
    set_label_if_changed(duration_time_lbl, buf);
}

lv_obj_t * music_ui_album_art(void)
{
    return cover;
}

void music_ui_clear_album_art(void)
{
    if(cover) lv_image_set_src(cover, nullptr);
    if(art_data) {
        lv_image_cache_drop(&art_dsc);
        free(art_data);
        art_data = nullptr;
    }
}

/* Copy the buffer, then point the cover at it. The old image is released first
 * so LVGL never draws from freed memory. */
static void set_album_art(const void * data, size_t size, const lv_image_header_t * header)
{
    if(!cover || !data || size == 0) return;

    unsigned char * copy = (unsigned char *)malloc(size);
    if(!copy) return;
    memcpy(copy, data, size);

    music_ui_clear_album_art();

    art_data = copy;
    memset(&art_dsc, 0, sizeof(art_dsc));
    art_dsc.header       = *header;
    art_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    art_dsc.data         = art_data;
    art_dsc.data_size    = (uint32_t)size;
    lv_image_set_src(cover, &art_dsc);
}

void music_ui_set_album_art_encoded(const void * data, size_t size)
{
    lv_image_header_t header;
    memset(&header, 0, sizeof(header));
    header.cf = LV_COLOR_FORMAT_RAW;
    set_album_art(data, size, &header);
}

void music_ui_set_album_art_raw(const void * data, uint32_t w, uint32_t h, lv_color_format_t cf)
{
    lv_image_header_t header;
    memset(&header, 0, sizeof(header));
    header.cf     = cf;
    header.w      = w;
    header.h      = h;
    header.stride = w * lv_color_format_get_size(cf);
    set_album_art(data, (size_t)h * header.stride, &header);
}

void music_ui_create(void)
{
    lv_obj_t * screen = lv_screen_active();

    // 화면 배경
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x12151B), 0);

    // 미디어 카드 (cover + title + artists 그룹)
    lv_obj_t * media_card = lv_obj_create(screen);
    lv_obj_set_size(media_card, 280, 420);
    lv_obj_set_flex_flow(media_card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(media_card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_opa(media_card, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(media_card, 0, 0);
    lv_obj_set_style_pad_all(media_card, 6, 0);
    lv_obj_set_style_pad_row(media_card, 0, 0);
    lv_obj_align(media_card, LV_ALIGN_TOP_LEFT, 30, 30);

    // 커버 이미지 placeholder
    cover = lv_image_create(media_card);
    lv_obj_set_size(cover, MUSIC_UI_COVER_SIZE, MUSIC_UI_COVER_SIZE);
    lv_obj_set_style_radius(cover, 16, LV_STATE_DEFAULT);
    lv_obj_set_style_clip_corner(cover, true, 0);
    lv_image_set_inner_align(cover, LV_IMAGE_ALIGN_STRETCH);

    // 제목 clipping 컨테이너 (커버 폭과 동일)
    title_box = lv_obj_create(media_card);
    lv_obj_set_size(title_box, 250, 36);
    lv_obj_set_style_bg_opa(title_box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(title_box, 0, 0);
    lv_obj_set_style_pad_all(title_box, 0, 0);
    lv_obj_set_style_clip_corner(title_box, true, 0);
    lv_obj_set_style_radius(title_box, 0, 0);
    lv_obj_set_scrollable(title_box, false);
    lv_obj_set_scrollbar_mode(title_box, LV_SCROLLBAR_MODE_OFF);

    // 곡 제목 (컨테이너 안에서 움직임)
    title = lv_label_create(title_box);
    lv_label_set_text(title, "");
    lv_obj_set_width(title, 250);
    lv_label_set_long_mode(title, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_style_text_font(title, noto_font(24, &lv_font_montserrat_28), LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), LV_STATE_DEFAULT);
    // 스크롤 속도(느림)
    lv_obj_set_style_anim_duration(title, 12000, LV_PART_MAIN);

    // 가수/그룹 (제목 아래, 조금 작게)
    artists = lv_label_create(media_card);
    lv_label_set_text(artists, "");
    // 커버 폭에 고정하고, 길면 그대로 잘라냄 (안 그러면 카드 폭을 넘겨 레이아웃이 밀림)
    lv_obj_set_width(artists, MUSIC_UI_COVER_SIZE);
    lv_label_set_long_mode(artists, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_style_text_font(artists, noto_font(16, &lv_font_montserrat_16), LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(artists, lv_color_hex(0xA0A0A0), LV_STATE_DEFAULT);

    // 재생 진행바 패널
    progress_panel = lv_obj_create(media_card);
    lv_obj_set_size(progress_panel, 250, 34);
    lv_obj_set_style_bg_opa(progress_panel, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(progress_panel, 0, 0);
    lv_obj_set_style_pad_all(progress_panel, 0, 0);
    lv_obj_set_scrollable(progress_panel, false);

    // 재생바: 패널 전체 폭
    progress_bar = lv_bar_create(progress_panel);
    lv_obj_set_size(progress_bar, 250, 6);
    lv_obj_align(progress_bar, LV_ALIGN_TOP_MID, 0, 6);
    lv_bar_set_range(progress_bar, 0, 1000);
    lv_obj_set_style_bg_color(progress_bar, lv_color_white(), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(progress_bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(progress_bar, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(progress_bar, LV_OPA_30, LV_PART_MAIN);
    lv_bar_set_value(progress_bar, 0, LV_ANIM_OFF);

    // 시간: 바 아래, 고정폭(monospace) 폰트
    current_time_lbl = lv_label_create(progress_panel);
    lv_label_set_text(current_time_lbl, "00:00");
    lv_obj_set_style_text_font(current_time_lbl, &font_mono_13, LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(current_time_lbl, lv_color_hex(0xFFFFFF), LV_STATE_DEFAULT);
    lv_obj_align(current_time_lbl, LV_ALIGN_TOP_LEFT, 0, 20);

    duration_time_lbl = lv_label_create(progress_panel);
    lv_label_set_text(duration_time_lbl, "00:00");
    lv_obj_set_style_text_font(duration_time_lbl, &font_mono_13, LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(duration_time_lbl, lv_color_hex(0xFFFFFF), LV_STATE_DEFAULT);
    lv_obj_align(duration_time_lbl, LV_ALIGN_TOP_RIGHT, 0, 20);

    // 재생 컨트롤 (재생바 아래, 카드 폭에 맞춤)
    lv_obj_t * controls = lv_obj_create(media_card);
    lv_obj_set_size(controls, 250, 64);
    lv_obj_set_flex_flow(controls, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(controls, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_opa(controls, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(controls, 0, 0);
    lv_obj_set_style_pad_all(controls, 0, 0);
    lv_obj_set_scrollable(controls, false);

    // 셔플 / 이전 / 재생 / 다음 / 반복 (카드 폭 250px에 한 줄)
    shuffle_label = create_transport_button(controls, LV_SYMBOL_SHUFFLE, 38, &lv_font_montserrat_16, MUSIC_UI_CMD_SHUFFLE);
    create_transport_button(controls, LV_SYMBOL_PREV, 46, &lv_font_montserrat_22, MUSIC_UI_CMD_PREVIOUS);
    play_label = create_transport_button(controls, LV_SYMBOL_PLAY, 60, &lv_font_montserrat_30, MUSIC_UI_CMD_PLAY_PAUSE);
    create_transport_button(controls, LV_SYMBOL_NEXT, 46, &lv_font_montserrat_22, MUSIC_UI_CMD_NEXT);
    repeat_label = create_transport_button(controls, LV_SYMBOL_LOOP, 38, &lv_font_montserrat_16, MUSIC_UI_CMD_REPEAT);

    repeat_one_label = lv_label_create(lv_obj_get_parent(repeat_label));
    lv_label_set_text(repeat_one_label, "1");
    lv_obj_set_style_text_font(repeat_one_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(repeat_one_label, lv_color_white(), 0);
    lv_obj_center(repeat_one_label);
    show_shuffle(false);
    show_repeat(MUSIC_UI_REPEAT_OFF);

    // 가사 영역 (오른쪽)
    lyrics_font  = noto_font(24, noto_font(24, &lv_font_montserrat_22), true); /* Bold; Regular for any missing glyph */
    lyrics_panel = lv_obj_create(screen);
    lv_obj_set_size(lyrics_panel, 420, 420);
    lv_obj_align(lyrics_panel, LV_ALIGN_TOP_RIGHT, -30, 30);
    lv_obj_set_flex_flow(lyrics_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_bg_opa(lyrics_panel, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(lyrics_panel, 0, 0);
    lv_obj_set_style_pad_row(lyrics_panel, 14, 0);
    lv_obj_set_style_pad_top(lyrics_panel, LYRICS_ANCHOR_Y, 0);          /* current line sits above the center */
    lv_obj_set_style_pad_bottom(lyrics_panel, 420 - LYRICS_ANCHOR_Y, 0); /* lets the last lines reach that spot */
    lv_obj_set_style_pad_hor(lyrics_panel, 8, 0);
    lv_obj_set_scrollbar_mode(lyrics_panel, LV_SCROLLBAR_MODE_OFF);
    music_ui_set_lyrics_message("");
    lv_timer_create(lyrics_tick_cb, 100, nullptr);
}
