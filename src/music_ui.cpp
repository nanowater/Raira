#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "lvgl_compat.h"
#include <src/misc/cache/instance/lv_image_cache.h>
#include "browse_ui.h"
#include "music_ui.h"
#include "settings_ui.h"
#include "strings.h"
#include "ui_fonts.h"

extern "C" const lv_font_t font_mono_16; /* DejaVu Sans Mono 16px, digits and ":" only (src/fonts) */
/* Noto Sans KR via TinyTTF (cached, see ui_fonts.cpp). */
static const lv_font_t * noto_font(int32_t px, const lv_font_t * fallback, bool bold = false)
{
    return ui_noto_font(px, fallback, bold);
}

static lv_obj_t * cover                     = nullptr;
static lv_obj_t * title_box                 = nullptr;
static lv_obj_t * title                     = nullptr;
static lv_obj_t * artists                   = nullptr;
static lv_obj_t * progress_panel            = nullptr;
static lv_obj_t * progress_bar              = nullptr; /* lv_slider: shows progress, drag/tap to seek */
static lv_obj_t * current_time_lbl          = nullptr;
static lv_obj_t * duration_time_lbl         = nullptr;
static lv_obj_t * quality_lbl               = nullptr; /* audio quality, between the two times */
static lv_timer_t * title_scroll_timer      = nullptr;
static lv_obj_t * controls                  = nullptr;
static lv_obj_t * cover_box                 = nullptr; /* rounded-corner clip around the cover image */
static lv_obj_t * play_label                = nullptr;
static music_ui_transport_cb_t transport_cb = nullptr;
static music_ui_seek_cb_t seek_cb           = nullptr;
static int g_duration_sec                   = 0;
static lv_obj_t * offline_banner            = nullptr;

/* After a click the icon flips right away; polled status is ignored until it agrees or the
 * grace period ends (tidal-hifi reports the old state for a moment). */
static const uint32_t OPTIMISTIC_GRACE_MS = 2000;
static const uint32_t ICON_ON             = 0xFFFFFF;
static const uint32_t ICON_OFF            = 0x6B7280;

struct Optimistic
{
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

static bool shown_playing = false;
static bool shown_shuffle = false;
static int shown_repeat   = MUSIC_UI_REPEAT_OFF;
static Optimistic opt_play, opt_shuffle, opt_repeat, opt_seek;
static int seek_target_sec         = 0;
static lv_obj_t * shuffle_label    = nullptr;
static lv_obj_t * repeat_label     = nullptr;
static lv_obj_t * repeat_one_label = nullptr; /* small "1" over the loop icon */
static lv_obj_t * btn_shuffle      = nullptr; /* buttons around the *_label icons */
static lv_obj_t * btn_prev         = nullptr;
static lv_obj_t * btn_next         = nullptr;
static lv_obj_t * btn_repeat       = nullptr;
/* Which controls the service can drive; absent ones are hidden (bitmask in music_ui.h). */
static unsigned supported_caps = ~0u; /* everything shown until music_ui_set_capabilities() */

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
    if(repeat_label)
        lv_obj_set_style_text_color(repeat_label, lv_color_hex(mode != MUSIC_UI_REPEAT_OFF ? ICON_ON : ICON_OFF), 0);
    if(repeat_one_label) lv_obj_set_hidden(repeat_one_label, mode != MUSIC_UI_REPEAT_SINGLE);
}

/* ---- Ambient background: two full-screen images; a new one fades in over the old one ---- */
static const uint32_t BG_FADE_MS = 800;
static lv_obj_t * bg_img[2]      = {nullptr, nullptr};
static unsigned char * bg_buf[2] = {nullptr, nullptr};
static lv_image_dsc_t bg_dsc[2];
static int bg_front = 0; /* the one currently on top / fully shown */

static void bg_opa_anim_cb(void * var, int32_t v)
{
    lv_obj_set_style_opa(static_cast<lv_obj_t *>(var), (lv_opa_t)v, 0);
}

void music_ui_set_background(const void * rgb565, uint32_t w, uint32_t h)
{
    if(!bg_img[0] || !rgb565 || w == 0 || h == 0) return;
    const size_t size    = (size_t)w * h * 2;
    unsigned char * copy = (unsigned char *)malloc(size);
    if(!copy) return;
    memcpy(copy, rgb565, size);

    /* The hidden image takes the new pixels; its previous buffer is released first. */
    const int back = 1 - bg_front;
    lv_anim_delete(bg_img[back], bg_opa_anim_cb);
    lv_obj_set_style_opa(bg_img[back], LV_OPA_TRANSP, 0);
    lv_image_set_src(bg_img[back], nullptr);
    if(bg_buf[back]) {
        lv_image_cache_drop(&bg_dsc[back]);
        free(bg_buf[back]);
    }
    bg_buf[back] = copy;
    memset(&bg_dsc[back], 0, sizeof(bg_dsc[back]));
    bg_dsc[back].header.magic  = LV_IMAGE_HEADER_MAGIC;
    bg_dsc[back].header.cf     = LV_COLOR_FORMAT_RGB565;
    bg_dsc[back].header.w      = w;
    bg_dsc[back].header.h      = h;
    bg_dsc[back].header.stride = w * 2;
    bg_dsc[back].data_size     = (uint32_t)size;
    bg_dsc[back].data          = bg_buf[back];
    lv_image_set_src(bg_img[back], &bg_dsc[back]);
    lv_obj_set_pos(bg_img[back], 0, 0);

    /* new image above the old one (both stay below every widget), fading in; the old one stays
     * fully opaque underneath until the new one covers it */
    lv_obj_move_to_index(bg_img[bg_front], 0);
    lv_obj_move_to_index(bg_img[back], 1);
    lv_obj_set_style_opa(bg_img[bg_front], LV_OPA_COVER, 0);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, bg_img[back]);
    lv_anim_set_exec_cb(&a, bg_opa_anim_cb);
    lv_anim_set_values(&a, 0, LV_OPA_COVER);
    lv_anim_set_duration(&a, BG_FADE_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_start(&a);
    bg_front = back;
}

/* Album art buffer (encoded PNG or decoded pixels) owned by the UI. */
static unsigned char * art_data = nullptr;
static lv_image_dsc_t art_dsc;

static void fmt_time(int sec, char * buf, size_t n)
{
    if(sec < 0) sec = 0;
    snprintf(buf, n, "%02d:%02d", sec / 60, sec % 60);
}

/* Title scroll speed in px/s. LVGL's anim_duration is the time for the whole travel, so a fixed
 * value makes long titles fly past; derive it from the text width instead. */
static const int32_t TITLE_SCROLL_PX_PER_S = 25;

static void apply_title_scroll_speed()
{
    const lv_font_t * font = lv_obj_get_style_text_font(title, LV_PART_MAIN);
    lv_point_t size;
    lv_text_get_size(&size, lv_label_get_text(title), font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    /* LVGL scrolls the text width plus LV_LABEL_WAIT_CHAR_COUNT spaces before wrapping around */
    int32_t distance = size.x + lv_font_get_glyph_width(font, ' ', ' ') * LV_LABEL_WAIT_CHAR_COUNT;
    int32_t ms       = (int32_t)((int64_t)distance * 1000 / TITLE_SCROLL_PX_PER_S);
    lv_obj_set_style_anim_duration(title, ms < 1000 ? 1000 : ms, LV_PART_MAIN);
}

static void enable_title_scroll_cb(lv_timer_t * t);

/* Show the title clipped, and let it scroll after a delay (also after a layout change). */
static void restart_title_scroll()
{
    lv_label_set_long_mode(title, LV_LABEL_LONG_MODE_CLIP);
    if(title_scroll_timer) lv_timer_del(title_scroll_timer);
    title_scroll_timer = lv_timer_create(enable_title_scroll_cb, 2000, nullptr);
}

static void enable_title_scroll_cb(lv_timer_t * t)
{
    if(title) {
        apply_title_scroll_speed();
        lv_label_set_long_mode(title, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
    }
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
static std::vector<bool> lyric_gap;            /* entry is an instrumental gap: a blank spacer, never highlighted */
static lv_obj_t * intro_gap_spacer  = nullptr; /* leading intro spacer that pulses */
static lv_obj_t * intro_gap_dots[3] = {nullptr, nullptr, nullptr};
static bool intro_dots_pulsing      = false; /* the dots currently run their pulse animation */
static bool lyrics_synced           = false;
static int lyric_cur                = -1;
static bool lyrics_available        = false; /* the current track has lyrics to show */
static bool lyrics_hidden_by_user   = false; /* swiped the lyrics away: keep the lyrics-free layout */
static bool browse_open             = false; /* the playlist/queue panel replaces the lyrics panel */
static lv_obj_t * browse_panel      = nullptr;
static lv_obj_t * browse_btn        = nullptr; /* toggles the panel; bottom right */
static lv_obj_t * browse_btn_label  = nullptr;

/* Playback position, interpolated between the 1 s polls so line changes feel on time. */
static bool pos_playing    = false;
static int32_t pos_base_ms = 0;
static uint32_t pos_tick   = 0;

static const uint32_t LYRIC_COLOR_DIM = 0x9CA3AF; /* readable on the ambient background too */
static const int32_t LYRICS_PANEL_H   = 420;
static const int32_t LYRICS_ANCHOR_Y  = 130; /* px from the panel top where the current line rests */
static const int32_t LYRICS_FONT_PX   = 28;

static int32_t position_ms()
{
    return pos_base_ms + (pos_playing ? (int32_t)lv_tick_elaps(pos_tick) : 0);
}

static void lyric_scroll_anim_cb(void * var, int32_t v);

static void lyrics_clear_labels()
{
    /* Forget the labels before deleting them: lv_obj_clean() scrolls the panel back while it removes
     * children, and the scroll handler walks lyric_labels, which must not hold deleted objects. */
    lyric_labels.clear();
    lyric_times.clear();
    lyric_gap.clear();
    intro_gap_spacer   = nullptr;
    intro_dots_pulsing = false;
    for(int k = 0; k < 3; k++) intro_gap_dots[k] = nullptr;
    lyric_cur = -1;
    if(lyrics_panel) {
        lv_anim_delete(lyrics_panel, lyric_scroll_anim_cb); /* a glide for the previous lyrics must not carry over */
        lv_obj_clean(lyrics_panel);
    }
}

/* Fade lines out near the top and bottom edge of the panel instead of cutting them off, so the
 * whole panel height is used for lyrics. Runs on every scroll step and after layout changes. */
static const int32_t LYRICS_EDGE_FADE_PX = 60;

static void lyrics_update_edge_fade()
{
    if(!lyrics_panel) return;
    const int32_t h      = lv_obj_get_height(lyrics_panel);
    const int32_t top    = lv_obj_get_style_pad_top(lyrics_panel, LV_PART_MAIN);
    const int32_t scroll = lv_obj_get_scroll_y(lyrics_panel);
    for(lv_obj_t * l : lyric_labels) {
        if(!l) continue; /* gap entry */
        int32_t center = lv_obj_get_y(l) + top - scroll + lv_obj_get_height(l) / 2;
        int32_t edge   = center < h - center ? center : h - center; /* distance to the nearer edge */
        int32_t opa    = edge * 255 / LYRICS_EDGE_FADE_PX;
        lv_obj_set_style_opa(l, (lv_opa_t)(opa < 0 ? 0 : opa > 255 ? 255 : opa), 0);
    }
}

static void lyrics_scroll_cb(lv_event_t *)
{
    lyrics_update_edge_fade();
}

/* Smooth highlight changes: the text color eases between dim and white, and the view glides to
 * the new line with an ease-in-out curve instead of jumping. */
static const uint32_t LYRIC_COLOR_FADE_MS = 250;
static const uint32_t LYRIC_SCROLL_MS     = 450;

static void lyric_color_anim_cb(void * var, int32_t v)
{
    lv_obj_set_style_text_color(static_cast<lv_obj_t *>(var),
                                lv_color_mix(lv_color_white(), lv_color_hex(LYRIC_COLOR_DIM), (uint8_t)v), 0);
}

static void animate_lyric_color(lv_obj_t * l, bool to_white)
{
    /* continue from wherever a running fade currently is, so quick line changes don't flicker */
    lv_anim_t * running = lv_anim_get(l, lyric_color_anim_cb);
    int32_t from        = running ? running->current_value : (to_white ? 0 : 255);
    lv_anim_delete(l, lyric_color_anim_cb);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, l);
    lv_anim_set_exec_cb(&a, lyric_color_anim_cb);
    lv_anim_set_values(&a, from, to_white ? 255 : 0);
    lv_anim_set_duration(&a, LYRIC_COLOR_FADE_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

static void lyric_scroll_anim_cb(void * var, int32_t v)
{
    lv_obj_scroll_to_y(static_cast<lv_obj_t *>(var), v, LV_ANIM_OFF);
}

static void glide_lyrics_to(int32_t y)
{
    lv_anim_delete(lyrics_panel, lyric_scroll_anim_cb);
    int32_t from = lv_obj_get_scroll_y(lyrics_panel);
    if(from == y) return;

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, lyrics_panel);
    lv_anim_set_exec_cb(&a, lyric_scroll_anim_cb);
    lv_anim_set_values(&a, from, y);
    lv_anim_set_duration(&a, LYRIC_SCROLL_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_start(&a);
}

/* Highlight line `idx`. Before the first line (idx < 0) and in gaps nothing is highlighted, but the
 * view still moves to the next line to be sung (or the last line once the lyrics are over), so
 * seeking back to the start or ahead into the outro scrolls along. */
static void lyrics_highlight(int idx)
{
    if(idx == lyric_cur) return;
    if(lyric_cur >= 0 && lyric_cur < (int)lyric_labels.size() && !lyric_gap[lyric_cur])
        animate_lyric_color(lyric_labels[lyric_cur], false);
    lyric_cur = idx;

    const int n        = (int)lyric_labels.size();
    bool leading_intro = (idx == 0 && n > 1 && !lyric_gap.empty() && lyric_gap[0] && lyric_times.size() > 1 &&
                          lyric_times[0] == 0 && lyric_times[1] > 0);
    lv_obj_t * focus   = (idx >= 0 && idx < n && !lyric_gap[idx]) ? lyric_labels[idx] : nullptr;
    if(leading_intro) {
        /* Intro: keep the pulsing dots on top, don't highlight the upcoming line yet. */
        glide_lyrics_to(0);
        return;
    }
    if(focus) {
        animate_lyric_color(focus, true);
    } else {
        /* gap / before the first line: aim at the next line that will be sung; after the last line
         * there is none, so stay on the last line (also when seeking into the outro) */
        for(int i = idx + 1; i < n && !focus; i++)
            if(!lyric_gap[i]) focus = lyric_labels[i];
        for(int i = n - 1; i >= 0 && !focus; i--)
            if(!lyric_gap[i]) focus = lyric_labels[i];
        if(!focus) return;
    }

    lv_obj_update_layout(lyrics_panel);
    /* lv_obj_get_y() is relative to the content area (below the top padding), so scrolling by it
     * leaves the line at LYRICS_ANCHOR_Y from the panel top. */
    glide_lyrics_to(lv_obj_get_y(focus));
}

/* ---- Intro dots: three round dots pulsing in sequence while the song has not started singing ---- */
static void intro_gap_opa_cb(void * var, int32_t v)
{
    lv_obj_set_style_opa(static_cast<lv_obj_t *>(var), (lv_opa_t)v, 0);
}

static lv_obj_t * intro_dots_create()
{
    lv_obj_t * row = lv_obj_create(lyrics_panel);
    lv_obj_set_size(row, lv_pct(100), 40); /* one lyric line tall */
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_pad_column(row, 12, 0);
    lv_obj_set_scrollable(row, false);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    for(int k = 0; k < 3; k++) {
        lv_obj_t * dot = lv_obj_create(row);
        lv_obj_set_size(dot, 12, 12);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(dot, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(dot, 0, 0);
        lv_obj_set_style_opa(dot, (lv_opa_t)120, 0);
        lv_obj_set_scrollable(dot, false);
        intro_gap_dots[k] = dot;
    }
    intro_gap_spacer = row;
    return row;
}

/* Starts/stops the pulse: only in the intro, only while playing. The resting opacity (a little brighter
 * while paused inside the intro, dim once the singing started) is set by lyrics_tick_cb. */
static void intro_dots_pulse(bool pulse)
{
    if(!intro_gap_dots[0]) {
        intro_dots_pulsing = false;
        return;
    }
    if(pulse == intro_dots_pulsing) return;
    intro_dots_pulsing = pulse;
    for(int k = 0; k < 3; k++) {
        lv_obj_t * dot = intro_gap_dots[k];
        lv_anim_delete(dot, intro_gap_opa_cb);
        if(pulse) {
            lv_anim_t a;
            lv_anim_init(&a);
            lv_anim_set_var(&a, dot);
            lv_anim_set_values(&a, 90, 255);
            lv_anim_set_duration(&a, 800);
            lv_anim_set_reverse_duration(&a, 800);
            lv_anim_set_delay(&a, k * 300);
            lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
            lv_anim_set_exec_cb(&a, intro_gap_opa_cb);
            lv_anim_start(&a);
        }
    }
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
    /* The dots stay in the list like a normal line; they pulse only during the intro while playing. */
    intro_dots_pulse(idx == 0 && pos_playing);
    if(intro_gap_dots[0] && !intro_dots_pulsing) /* still: brighter while paused in the intro, dim afterwards */
        for(int k = 0; k < 3; k++) lv_obj_set_style_opa(intro_gap_dots[k], (lv_opa_t)(idx == 0 ? 200 : 120), 0);
    lyrics_highlight(idx);
}

void music_ui_set_lyrics_message(const char * msg)
{
    if(!lyrics_panel) return;
    lyrics_available = false; /* loading / not found */
    lyrics_synced    = false;
    lyrics_clear_labels();
    lv_obj_t * l = lv_label_create(lyrics_panel);
    lv_label_set_text(l, msg ? msg : "");
    lv_obj_set_style_text_font(l, lyrics_font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(LYRIC_COLOR_DIM), 0);
}

/* Layouts (see "Layouts" below): LYRICS = small cover + lyrics, NOW_PLAYING = big cover, no lyrics. */
enum { LAYOUT_LYRICS = 0, LAYOUT_NOW_PLAYING = 1 };
static void request_layout(int mode);

void music_ui_set_lyrics(const music_ui_lyric_line_t * lines, size_t count, bool synced)
{
    if(!lyrics_panel) return;
    lyrics_clear_labels();
    lyrics_synced = synced && count > 0;
    lv_obj_scroll_to_y(lyrics_panel, 0, LV_ANIM_OFF);

    /* Inject a leading intro gap when the first sung line starts well after the track begins, so the
     * opening instrumental is shown as a pulsing dotted line instead of immediately highlighting. */
    std::vector<music_ui_lyric_line_t> all;
    if(synced && count > 0 && lines[0].time_ms >= 4000) {
        music_ui_lyric_line_t g{};
        g.text    = nullptr;
        g.time_ms = 0;
        all.push_back(g);
    }
    for(size_t i = 0; i < count; i++) all.push_back(lines[i]);

    for(size_t i = 0; i < all.size(); i++) {
        /* Empty text = timing-only gap entry (intro, interlude, after the last line). It is shown as
         * one blank line so the lyrics visibly pause there; consecutive gaps share that spacer. */
        if(synced && (!all[i].text || !all[i].text[0])) {
            lv_obj_t * spacer = nullptr;
            if(!lyric_gap.empty() && lyric_gap.back()) {
                spacer = lyric_labels.back();
            } else {
                if(i == 0) {
                    /* Intro gap: three dots pulsing in sequence, occupying one line. */
                    spacer = intro_dots_create();
                } else {
                    spacer = lv_label_create(lyrics_panel);
                    lv_obj_set_width(spacer, lv_pct(100));
                    lv_obj_set_style_text_font(spacer, lyrics_font, 0);
                    lv_obj_set_style_text_color(spacer, lv_color_white(), 0);
                    lv_label_set_text(spacer, " ");
                }
            }
            lyric_labels.push_back(spacer);
            lyric_times.push_back(all[i].time_ms);
            lyric_gap.push_back(true);
            continue;
        }
        lv_obj_t * l = lv_label_create(lyrics_panel);
        lv_obj_set_width(l, lv_pct(100));
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
        lv_label_set_text(l, all[i].text ? all[i].text : "");
        lv_obj_set_style_text_font(l, lyrics_font, 0);
        lv_obj_set_style_text_color(l, lv_color_hex(synced ? LYRIC_COLOR_DIM : 0xD1D5DB), 0);
        lyric_labels.push_back(l);
        lyric_times.push_back(all[i].time_ms);
        lyric_gap.push_back(false);
    }
    lv_obj_update_layout(lyrics_panel);
    lyrics_update_edge_fade();
    if(lyrics_synced) lyrics_tick_cb(nullptr);
    lyrics_available = true;
    request_layout(lyrics_hidden_by_user && !browse_open ? LAYOUT_NOW_PLAYING : LAYOUT_LYRICS);
}

static void lyrics_sync_position(const music_ui_state_t * st, bool hold)
{
    if(hold) { /* a seek is in flight: keep our own position until the polled one catches up */
        pos_playing = st->playing;
        return;
    }
    /* A source that knows the position in ms gets it as it is and may drift only a little before we resync; one that
     * reports whole seconds (tidal-hifi direct) is taken at the middle of its second, +-1 s. */
    const bool precise      = st->position_ms >= 0;
    const int32_t reported  = precise ? st->position_ms : st->current_sec * 1000 + 500;
    const int32_t tolerance = precise ? 300 : 1000;
    bool keep               = pos_playing && st->playing && std::abs(position_ms() - reported) <= tolerance;
    pos_playing             = st->playing;
    if(!keep) {
        pos_base_ms = reported;
        pos_tick    = lv_tick_get();
    }
}

void music_ui_set_connected(bool connected)
{
    if(offline_banner) lv_obj_set_hidden(offline_banner, connected);
    if(!connected && pos_playing) {
        /* Nobody reports the position any more: stop our own clock, or the progress bar and the lyrics keep running
         * on a song that may have stopped long ago. The next update after a reconnect restarts it. */
        pos_base_ms = position_ms();
        pos_tick    = lv_tick_get();
        pos_playing = false;
        show_playing(false);
    }
}

/* Show only the controls the service can drive (the flex row reflows on its own); without seek the
 * progress bar keeps displaying position but stops reacting to touches. */
static void set_browse_open(bool open);

void music_ui_clear_content(void)
{
    g_duration_sec = 0;
    pos_base_ms    = 0;
    pos_tick       = lv_tick_get();
    set_label_if_changed(title, "");
    set_label_if_changed(artists, "");
    set_label_if_changed(current_time_lbl, "00:00");
    set_label_if_changed(duration_time_lbl, "00:00");
    set_label_if_changed(quality_lbl, "");
    if(progress_bar) lv_slider_set_value(progress_bar, 0, LV_ANIM_OFF);
    music_ui_clear_album_art();
    for(lv_obj_t * bg : bg_img) { /* show the plain base color; the next background fades in from there */
        if(!bg) continue;
        lv_anim_delete(bg, bg_opa_anim_cb);
        lv_obj_set_style_opa(bg, LV_OPA_TRANSP, 0);
    }
    music_ui_set_lyrics_message(""); /* the layout stays as it is: it animates when the lyrics come back */
}

void music_ui_set_capabilities(unsigned caps)
{
    supported_caps = caps;
    if(progress_bar) lv_obj_set_clickable(progress_bar, supported_caps & MUSIC_UI_CAP_SEEK);
    if(btn_shuffle) lv_obj_set_hidden(btn_shuffle, !(supported_caps & MUSIC_UI_CAP_SHUFFLE));
    if(btn_repeat) lv_obj_set_hidden(btn_repeat, !(supported_caps & MUSIC_UI_CAP_REPEAT));
    if(btn_prev) lv_obj_set_hidden(btn_prev, !(supported_caps & MUSIC_UI_CAP_PREV_NEXT));
    if(btn_next) lv_obj_set_hidden(btn_next, !(supported_caps & MUSIC_UI_CAP_PREV_NEXT));
    if(browse_btn) lv_obj_set_hidden(browse_btn, !(supported_caps & MUSIC_UI_CAP_BROWSE));
    if(!(supported_caps & MUSIC_UI_CAP_BROWSE)) set_browse_open(false); /* the player changed: no list to show */

    /* SPACE_BETWEEN pushes a lone button to the left edge: with only play/pause left, center it */
    if(controls) {
        int visible = 1; /* play/pause is always shown */
        for(lv_obj_t * b : {btn_shuffle, btn_prev, btn_next, btn_repeat})
            if(b && !lv_obj_is_hidden(b)) visible++;
        lv_obj_set_flex_align(controls, visible <= 1 ? LV_FLEX_ALIGN_CENTER : LV_FLEX_ALIGN_SPACE_BETWEEN,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    }
}

void music_ui_set_seek_cb(music_ui_seek_cb_t cb)
{
    seek_cb = cb;
}

static int slider_to_seconds()
{
    return (int)((int64_t)lv_slider_get_value(progress_bar) * g_duration_sec / 1000);
}

/* While dragging, preview the target time next to the bar. */
static void on_progress_changed(lv_event_t *)
{
    if(g_duration_sec <= 0 || !lv_slider_is_dragged(progress_bar)) return;
    char buf[16];
    fmt_time(slider_to_seconds(), buf, sizeof(buf));
    lv_label_set_text(current_time_lbl, buf);
}

/* Let go: send the seek, move our own clock (progress, lyrics) right away and ignore polled
 * positions until they agree or the grace period ends. */
static void on_progress_released(lv_event_t *)
{
    if(g_duration_sec <= 0 || !(supported_caps & MUSIC_UI_CAP_SEEK)) return;
    int sec         = slider_to_seconds();
    seek_target_sec = sec;
    opt_seek.start();
    pos_base_ms = sec * 1000 + 500;
    pos_tick    = lv_tick_get();
    lyrics_tick_cb(nullptr);
    if(seek_cb) seek_cb(sec);
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

/* The row buttons change size and icon font with the layout. */
struct TransportBtn
{
    lv_obj_t * btn;
    lv_obj_t * lbl;
    int32_t size[2];
    const lv_font_t * font[2];
};
static std::vector<TransportBtn> transport_btns;

/* Round icon button using a built-in LVGL symbol. Returns the label (for icon swaps). */
static lv_obj_t * create_transport_button(lv_obj_t * parent, const char * symbol, int32_t size, const lv_font_t * font,
                                          int32_t big_size, const lv_font_t * big_font, music_ui_cmd_t cmd)
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
    transport_btns.push_back({btn, lbl, {size, big_size}, {font, big_font}});
    return lbl;
}

void music_ui_update(const music_ui_state_t * st)
{
    if(!st) return;
    g_duration_sec       = st->duration_sec;
    const bool dragging  = progress_bar && lv_slider_is_dragged(progress_bar);
    const bool hold_seek = dragging || opt_seek.holds(std::abs(st->current_sec - seek_target_sec) <= 2);
    lyrics_sync_position(st, hold_seek);
    if(!opt_play.holds(st->playing == shown_playing)) show_playing(st->playing);
    if(!opt_shuffle.holds(st->shuffle == shown_shuffle)) show_shuffle(st->shuffle);
    if(!opt_repeat.holds(st->repeat == shown_repeat)) show_repeat(st->repeat);
    const char * new_title   = st->title ? st->title : "";
    const char * new_artists = st->artists ? st->artists : "";

    if(title && strcmp(lv_label_get_text(title), new_title) != 0) {
        /* New track: restart from a clipped label, then enable scrolling after a delay. */
        lv_label_set_text(title, new_title);
        restart_title_scroll();
    }
    set_label_if_changed(artists, new_artists);

    if(progress_bar && !hold_seek) {
        int v = 0;
        if(st->duration_sec > 0) {
            v = (int)(((int64_t)st->current_sec * 1000) / st->duration_sec);
            if(v < 0) v = 0;
            if(v > 1000) v = 1000;
        }
        lv_slider_set_value(progress_bar, v, LV_ANIM_OFF);
    }

    char buf[16];
    if(!hold_seek) {
        fmt_time(st->current_sec, buf, sizeof(buf));
        set_label_if_changed(current_time_lbl, buf);
    }
    fmt_time(st->duration_sec, buf, sizeof(buf));
    set_label_if_changed(duration_time_lbl, buf);
    const char * quality = st->quality ? st->quality : "";
    set_label_if_changed(quality_lbl, quality);
    if(quality_lbl) {
        /* plain gray for compressed tiers, cyan for lossless, gold for hi-res */
        uint32_t color = st->quality_tier == MUSIC_UI_QUALITY_HIRES      ? 0xE5B94A
                         : st->quality_tier == MUSIC_UI_QUALITY_LOSSLESS ? 0x5CC8E8
                                                                         : 0xA0A0A0;
        lv_obj_set_style_text_color(quality_lbl, lv_color_hex(color), 0);
    }
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

/* ---- Layouts ----
 * LYRICS:      small cover, info below it, lyrics panel on the right.
 * NOW_PLAYING: big cover on the left, title/artists/progress/controls on the right (no lyrics). */
static int layout_mode                   = -1;
static const lv_font_t * title_font[2]   = {nullptr, nullptr};
static const lv_font_t * artists_font[2] = {nullptr, nullptr};

struct Geo
{
    int32_t cover_x, cover_y, cover_size; /* cover widget */
    int32_t x, w;                         /* column holding title, artists, progress, controls */
    int32_t title_y, title_h, artists_y, prog_y, prog_h, ctrl_y, ctrl_h;
};
static const Geo GEO[2] = {
    /* LYRICS      */ {45, 24, 250, 45, 250, 274, 40, 314, 342, 46, 388, 68},
    /* NOW_PLAYING */ {30, 40, 400, 470, 300, 114, 54, 172, 224, 52, 286, 80},
};

/* The right-hand area (LYRICS layout only) shows either the lyrics or the playlist/queue panel. */
static void update_right_panels(int mode)
{
    lv_obj_set_hidden(lyrics_panel, mode != LAYOUT_LYRICS || browse_open);
    if(browse_panel) lv_obj_set_hidden(browse_panel, mode != LAYOUT_LYRICS || !browse_open);
}

/* Column = everything that is not the cover: title, artists, progress, controls, lyrics/browse panel. */
static void apply_column(int mode)
{
    const Geo & g = GEO[mode];

    lv_obj_set_size(title_box, g.w, g.title_h);
    lv_obj_set_pos(title_box, g.x, g.title_y);
    lv_obj_set_width(title, g.w);
    lv_obj_set_style_text_font(title, title_font[mode], LV_STATE_DEFAULT);
    restart_title_scroll(); /* the speed depends on the font and the width */

    lv_obj_set_width(artists, g.w);
    lv_obj_set_pos(artists, g.x, g.artists_y);
    lv_obj_set_style_text_font(artists, artists_font[mode], LV_STATE_DEFAULT);

    lv_obj_set_size(progress_panel, g.w, g.prog_h);
    lv_obj_set_pos(progress_panel, g.x, g.prog_y);
    lv_obj_set_width(progress_bar, g.w);

    lv_obj_set_size(controls, g.w, g.ctrl_h);
    lv_obj_set_pos(controls, g.x, g.ctrl_y);
    for(const TransportBtn & b : transport_btns) {
        lv_obj_set_size(b.btn, b.size[mode], b.size[mode]);
        lv_obj_set_style_text_font(b.lbl, b.font[mode], 0);
    }

    update_right_panels(mode);
}

static void set_column_opa(int32_t opa)
{
    for(lv_obj_t * o : {title_box, artists, progress_panel, controls, lyrics_panel, browse_panel})
        if(o) lv_obj_set_style_opa(o, (lv_opa_t)opa, 0);
}

static void set_cover_geo(const Geo & a, const Geo & b, float t)
{
    auto lerp = [t](int32_t x, int32_t y) { return (int32_t)(x + (y - x) * t + 0.5f); };
    lv_obj_set_size(cover_box, lerp(a.cover_size, b.cover_size), lerp(a.cover_size, b.cover_size));
    lv_obj_set_pos(cover_box, lerp(a.cover_x, b.cover_x), lerp(a.cover_y, b.cover_y));
}

/* ---- Layout transition: the cover glides and grows/shrinks; the column fades out, is rearranged
 * while invisible, and fades back in ---- */
static const uint32_t LAYOUT_ANIM_MS = 420;
static const float COLUMN_SWITCH_AT  = 0.4f; /* fraction of the animation where the column is rearranged */
static bool layout_animating         = false;
static bool column_switched          = false;
static int layout_from               = LAYOUT_LYRICS;
static int layout_to                 = LAYOUT_LYRICS;

static void layout_anim_cb(void *, int32_t v)
{
    const float t = v / 1000.f;
    set_cover_geo(GEO[layout_from], GEO[layout_to], t);
    if(t < COLUMN_SWITCH_AT) {
        set_column_opa((int32_t)(255 * (1.f - t / COLUMN_SWITCH_AT)));
    } else {
        if(!column_switched) {
            apply_column(layout_to);
            column_switched = true;
        }
        set_column_opa((int32_t)(255 * (t - COLUMN_SWITCH_AT) / (1.f - COLUMN_SWITCH_AT)));
    }
}

static void finish_layout_transition()
{
    lv_anim_delete(lv_screen_active(), layout_anim_cb);
    set_cover_geo(GEO[layout_to], GEO[layout_to], 0.f);
    if(!column_switched) apply_column(layout_to);
    set_column_opa(255);
    layout_mode      = layout_to;
    layout_animating = false;
}

static void layout_anim_ready(lv_anim_t *)
{
    finish_layout_transition();
}

static void apply_layout_now(int mode) /* no animation: initial layout */
{
    layout_from = layout_to = layout_mode = mode;
    set_cover_geo(GEO[mode], GEO[mode], 0.f);
    apply_column(mode);
    set_column_opa(255);
}

static void request_layout(int mode)
{
    if(!cover) return;
    if(layout_animating) {
        if(mode == layout_to) return;
        finish_layout_transition(); /* jump to where it was heading, then animate to the new target */
    }
    if(mode == layout_mode) return;

    layout_from      = layout_mode;
    layout_to        = mode;
    layout_animating = true;
    column_switched  = false;

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, lv_screen_active());
    lv_anim_set_exec_cb(&a, layout_anim_cb);
    lv_anim_set_values(&a, 0, 1000);
    lv_anim_set_duration(&a, LAYOUT_ANIM_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_set_completed_cb(&a, layout_anim_ready);
    lv_anim_start(&a);
}

void music_ui_set_no_lyrics(void)
{
    music_ui_set_lyrics_message("");
    request_layout(browse_open ? LAYOUT_LYRICS : LAYOUT_NOW_PLAYING); /* the browse panel needs the narrow cover */
}

/* Open/close the playlist/queue panel in place of the lyrics. Opening forces the LYRICS layout (small
 * cover, right-hand area); closing returns to whatever the lyrics situation calls for. */
static void set_browse_open(bool open)
{
    if(open == browse_open || !browse_panel) return;
    browse_open = open;
    if(browse_btn_label) lv_label_set_text(browse_btn_label, open ? LV_SYMBOL_CLOSE : LV_SYMBOL_LIST);

    const int target =
        open ? LAYOUT_LYRICS : (lyrics_available && !lyrics_hidden_by_user ? LAYOUT_LYRICS : LAYOUT_NOW_PLAYING);
    const int current = layout_animating ? layout_to : layout_mode;
    if(target == current)
        update_right_panels(current); /* no layout change: swap the panels directly */
    else
        request_layout(target); /* the panels swap when the column is rearranged */
    if(open) browse_ui_opened();
}

/* Tapping the cover shows or hides the lyrics. The choice sticks across tracks; tracks without
 * lyrics always use the lyrics-free layout. With the browse panel open it just closes the panel. */
static void on_cover_clicked(lv_event_t *)
{
    if(browse_open) {
        set_browse_open(false);
        return;
    }
    const int current = layout_animating ? layout_to : layout_mode;
    if(current == LAYOUT_LYRICS) {
        lyrics_hidden_by_user = true;
        request_layout(LAYOUT_NOW_PLAYING);
    } else if(lyrics_available) {
        lyrics_hidden_by_user = false;
        request_layout(LAYOUT_LYRICS);
    }
}

void music_ui_create(void)
{
    /* Dark theme with the Korean-capable font as default: the settings page (list, text input, keyboard)
     * is built from stock widgets, which take their colors and font from the theme. */
    lv_display_set_theme(lv_display_get_default(),
                         lv_theme_default_init(lv_display_get_default(), lv_color_hex(0x60A5FA), lv_color_hex(0x3B82F6),
                                               true, noto_font(20, &lv_font_montserrat_22)));

    lv_obj_t * screen = lv_screen_active();

    // 화면 배경
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x12151B), 0);

    // 앨범아트 기반 앰비언트 배경 (가장 뒤에 깔림)
    for(int i = 0; i < 2; i++) {
        bg_img[i] = lv_image_create(screen);
        lv_obj_set_style_opa(bg_img[i], LV_OPA_TRANSP, 0);
        lv_obj_set_clickable(bg_img[i], false);
    }

    // 레이아웃에 따라 위치/크기가 바뀌므로 위젯은 화면에 직접 두고 apply_layout_now()/request_layout()이 배치한다
    title_font[LAYOUT_LYRICS]        = noto_font(28, &lv_font_montserrat_28);
    title_font[LAYOUT_NOW_PLAYING]   = noto_font(36, &lv_font_montserrat_36);
    artists_font[LAYOUT_LYRICS]      = noto_font(20, &lv_font_montserrat_22);
    artists_font[LAYOUT_NOW_PLAYING] = noto_font(26, &lv_font_montserrat_28);

    // 커버 이미지 placeholder
    /* The rounded corners live on a box around the image: clipping an image widget that LVGL scales
     * (400 px art shown at 250 px) loses the corner mask, a parent box clips whatever it contains. */
    cover_box = lv_obj_create(screen);
    lv_obj_set_style_bg_opa(cover_box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(cover_box, 0, 0);
    lv_obj_set_style_pad_all(cover_box, 0, 0);
    lv_obj_set_style_radius(cover_box, 16, LV_STATE_DEFAULT);
    lv_obj_set_style_clip_corner(cover_box, true, 0);
    lv_obj_set_scrollable(cover_box, false);
    cover = lv_image_create(cover_box);
    lv_obj_set_size(cover, LV_PCT(100), LV_PCT(100));
    lv_image_set_inner_align(cover, LV_IMAGE_ALIGN_STRETCH);
    lv_obj_set_clickable(cover, false);

    // 제목 clipping 컨테이너
    title_box = lv_obj_create(screen);
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
    lv_label_set_long_mode(title, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), LV_STATE_DEFAULT);

    // 가수/그룹 (제목 아래, 조금 작게). 폭에 고정하고 길면 그대로 잘라냄
    artists = lv_label_create(screen);
    lv_label_set_text(artists, "");
    lv_label_set_long_mode(artists, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_style_text_color(artists, lv_color_hex(0xA0A0A0), LV_STATE_DEFAULT);

    // 재생 진행바 패널
    progress_panel = lv_obj_create(screen);
    lv_obj_set_style_bg_opa(progress_panel, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(progress_panel, 0, 0);
    lv_obj_set_style_pad_all(progress_panel, 0, 0);
    lv_obj_set_scrollable(progress_panel, false);

    // 재생바: 패널 전체 폭
    progress_bar = lv_slider_create(progress_panel);
    lv_obj_set_height(progress_bar, 8);
    lv_obj_align(progress_bar, LV_ALIGN_TOP_MID, 0, 8);
    lv_obj_set_ext_click_area(progress_bar, 14); /* the bar is thin: make it easy to hit with a finger */
    lv_slider_set_range(progress_bar, 0, 1000);
    lv_obj_set_style_bg_color(progress_bar, lv_color_white(), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(progress_bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(progress_bar, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(progress_bar, LV_OPA_30, LV_PART_MAIN);
    /* knob only while touched */
    lv_obj_set_style_bg_color(progress_bar, lv_color_white(), LV_PART_KNOB);
    lv_obj_set_style_bg_opa(progress_bar, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_set_style_bg_opa(progress_bar, LV_OPA_COVER, (lv_style_selector_t)LV_PART_KNOB | LV_STATE_PRESSED);
    lv_obj_set_style_pad_all(progress_bar, 6, LV_PART_KNOB);
    lv_obj_set_style_shadow_width(progress_bar, 0, LV_PART_KNOB);
    lv_obj_set_style_border_width(progress_bar, 0, LV_PART_KNOB);
    lv_obj_add_event_cb(progress_bar, on_progress_changed, LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_add_event_cb(progress_bar, on_progress_released, LV_EVENT_RELEASED, nullptr);
    lv_slider_set_value(progress_bar, 0, LV_ANIM_OFF);

    // 시간: 바 아래, 고정폭(monospace) 폰트
    current_time_lbl = lv_label_create(progress_panel);
    lv_label_set_text(current_time_lbl, "00:00");
    lv_obj_set_style_text_font(current_time_lbl, &font_mono_16, LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(current_time_lbl, lv_color_hex(0xFFFFFF), LV_STATE_DEFAULT);
    lv_obj_align(current_time_lbl, LV_ALIGN_TOP_LEFT, 0, 22);

    duration_time_lbl = lv_label_create(progress_panel);
    lv_label_set_text(duration_time_lbl, "00:00");
    lv_obj_set_style_text_font(duration_time_lbl, &font_mono_16, LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(duration_time_lbl, lv_color_hex(0xFFFFFF), LV_STATE_DEFAULT);
    lv_obj_align(duration_time_lbl, LV_ALIGN_TOP_RIGHT, 0, 22);

    // 음질 (두 시간 사이, 가운데)
    quality_lbl = lv_label_create(progress_panel);
    lv_label_set_text(quality_lbl, "");
    lv_obj_set_style_text_font(quality_lbl, &lv_font_montserrat_14, LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(quality_lbl, lv_color_hex(0xA0A0A0), LV_STATE_DEFAULT);
    lv_obj_align(quality_lbl, LV_ALIGN_TOP_MID, 0, 22);

    // 재생 컨트롤 (재생바 아래, 컬럼 폭에 맞춤)
    controls = lv_obj_create(screen);
    lv_obj_set_flex_flow(controls, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(controls, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_opa(controls, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(controls, 0, 0);
    lv_obj_set_style_pad_all(controls, 0, 0);
    lv_obj_set_scrollable(controls, false);

    // 셔플 / 이전 / 재생 / 다음 / 반복 (한 줄, 크기는 레이아웃마다 다름: 작은 것 / 큰 것)
    /* create_transport_button() returns the icon label; the button is its parent (see repeat's "1"). */
    shuffle_label       = create_transport_button(controls, LV_SYMBOL_SHUFFLE, 40, &lv_font_montserrat_22, 52,
                                                  &lv_font_montserrat_28, MUSIC_UI_CMD_SHUFFLE);
    btn_shuffle         = lv_obj_get_parent(shuffle_label);
    lv_obj_t * prev_lbl = create_transport_button(controls, LV_SYMBOL_PREV, 48, &lv_font_montserrat_28, 62,
                                                  &lv_font_montserrat_36, MUSIC_UI_CMD_PREVIOUS);
    btn_prev            = lv_obj_get_parent(prev_lbl);
    play_label          = create_transport_button(controls, LV_SYMBOL_PLAY, 60, &lv_font_montserrat_36, 76,
                                                  &lv_font_montserrat_44, MUSIC_UI_CMD_PLAY_PAUSE);
    lv_obj_t * next_lbl = create_transport_button(controls, LV_SYMBOL_NEXT, 48, &lv_font_montserrat_28, 62,
                                                  &lv_font_montserrat_36, MUSIC_UI_CMD_NEXT);
    btn_next            = lv_obj_get_parent(next_lbl);
    repeat_label        = create_transport_button(controls, LV_SYMBOL_LOOP, 40, &lv_font_montserrat_22, 52,
                                                  &lv_font_montserrat_28, MUSIC_UI_CMD_REPEAT);
    btn_repeat          = lv_obj_get_parent(repeat_label);

    repeat_one_label = lv_label_create(lv_obj_get_parent(repeat_label));
    lv_label_set_text(repeat_one_label, "1");
    lv_obj_set_style_text_font(repeat_one_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(repeat_one_label, lv_color_white(), 0);
    lv_obj_center(repeat_one_label);
    show_shuffle(false);
    show_repeat(MUSIC_UI_REPEAT_OFF);

    // 가사 영역 (오른쪽)
    /* Bold; Regular for any missing glyph */
    lyrics_font  = noto_font(LYRICS_FONT_PX, noto_font(LYRICS_FONT_PX, &lv_font_montserrat_28), true);
    lyrics_panel = lv_obj_create(screen);
    lv_obj_set_size(lyrics_panel, 420, LYRICS_PANEL_H);
    lv_obj_align(lyrics_panel, LV_ALIGN_RIGHT_MID, -30, 0);
    lv_obj_set_flex_flow(lyrics_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_bg_opa(lyrics_panel, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(lyrics_panel, 0, 0);
    lv_obj_set_style_pad_row(lyrics_panel, 14, 0);
    lv_obj_set_style_pad_top(lyrics_panel, LYRICS_ANCHOR_Y, 0); /* current line sits above the center */
    lv_obj_set_style_pad_bottom(lyrics_panel, LYRICS_PANEL_H - LYRICS_ANCHOR_Y,
                                0); /* lets the last lines reach that spot */
    lv_obj_set_style_pad_hor(lyrics_panel, 8, 0);
    lv_obj_set_scrollbar_mode(lyrics_panel, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_event_cb(lyrics_panel, lyrics_scroll_cb, LV_EVENT_SCROLL, nullptr);
    music_ui_set_lyrics_message("");
    lv_timer_create(lyrics_tick_cb, 100, nullptr);

    // 플레이리스트/큐 패널 (가사 영역 자리에 대신 표시, 오른쪽 아래 버튼이 열고 닫음)
    browse_panel = browse_ui_create(screen, 420, 372);
    lv_obj_set_pos(browse_panel, 350, 24);
    apply_layout_now(LAYOUT_LYRICS);
    /* A short tap toggles the lyrics. LVGL sends CLICKED even after a long press, SHORT_CLICKED only
     * for a short one, so a long press can open the settings without also toggling the layout. */
    lv_obj_add_event_cb(cover_box, on_cover_clicked, LV_EVENT_SHORT_CLICKED, nullptr);
    lv_obj_add_event_cb(cover_box, [](lv_event_t *) { settings_ui_open_menu(); }, LV_EVENT_LONG_PRESSED, nullptr);

    // 연결 끊김 배너 (끊겼을 때만 표시, 오른쪽 위)
    offline_banner = lv_obj_create(screen);
    lv_obj_set_size(offline_banner, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_align(offline_banner, LV_ALIGN_TOP_RIGHT, -30, 11);
    lv_obj_set_flex_flow(offline_banner, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(offline_banner, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_color(offline_banner, lv_color_hex(0x1F2937), 0);
    lv_obj_set_style_bg_opa(offline_banner, LV_OPA_90, 0);
    lv_obj_set_style_border_width(offline_banner, 0, 0);
    lv_obj_set_style_radius(offline_banner, 14, 0);
    lv_obj_set_style_pad_hor(offline_banner, 12, 0);
    lv_obj_set_style_pad_ver(offline_banner, 4, 0);
    lv_obj_set_style_pad_column(offline_banner, 8, 0);
    lv_obj_set_scrollable(offline_banner, false);

    lv_obj_t * dot = lv_obj_create(offline_banner);
    lv_obj_set_size(dot, 10, 10);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dot, lv_color_hex(0xEF4444), 0);
    lv_obj_set_style_border_width(dot, 0, 0);

    lv_obj_t * offline_text = lv_label_create(offline_banner);
    lv_label_set_text(offline_text, str::CONNECTION_LOST);
    lv_obj_set_style_text_font(offline_text, noto_font(18, &lv_font_montserrat_16), 0);
    lv_obj_set_style_text_color(offline_text, lv_color_white(), 0);

    /* Tapping the banner is the way to fix the connection: it opens the settings. */
    lv_obj_add_event_cb(offline_banner, [](lv_event_t *) { settings_ui_open(); }, LV_EVENT_CLICKED, nullptr);
    lv_obj_set_hidden(offline_banner, true); /* shown only while the host is unreachable */

    /* Browse toggle: bottom right, free in both layouts. Hidden until a provider exists. */
    browse_btn = lv_obj_create(screen);
    lv_obj_set_size(browse_btn, 52, 52);
    lv_obj_set_pos(browse_btn, 718, 408);
    lv_obj_set_style_radius(browse_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(browse_btn, lv_color_hex(0x1F2937), 0);
    lv_obj_set_style_bg_opa(browse_btn, LV_OPA_90, 0);
    lv_obj_set_style_border_width(browse_btn, 0, 0);
    lv_obj_set_style_pad_all(browse_btn, 0, 0);
    lv_obj_set_scrollable(browse_btn, false);
    browse_btn_label = lv_label_create(browse_btn);
    lv_label_set_text(browse_btn_label, LV_SYMBOL_LIST);
    lv_obj_set_style_text_font(browse_btn_label, &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_color(browse_btn_label, lv_color_white(), 0);
    lv_obj_center(browse_btn_label);
    lv_obj_add_event_cb(browse_btn, [](lv_event_t *) { set_browse_open(!browse_open); }, LV_EVENT_CLICKED, nullptr);
    lv_obj_set_hidden(browse_btn, true); /* until music_ui_set_capabilities() says the service can browse */
}

void music_ui_set_browse_cb(browse_cb_t cb)
{
    browse_ui_set_cb(cb);
}
