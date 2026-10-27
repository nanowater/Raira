/* The playlist / queue panel (browse_ui.h): two tabs, a drill-down from a playlist to its tracks, rows addressed by
 * index. Rebuilds run from a 1 ms timer so that a row that was tapped is never deleted inside its own event.
 * Thumbnails: rows ask the provider only for what is on screen or one row away, and a cache keeps a bounded number of
 * the others. */
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <src/misc/cache/instance/lv_image_cache.h>
#include "browse_ui.h"
#include "strings.h"
#include "ui_fonts.h"

namespace {

struct Item
{
    std::string title, subtitle;
    bool playable = false;
    bool current  = false;
    std::string cover;
};

struct Thumb
{
    unsigned char * buf = nullptr;
    lv_image_dsc_t dsc;
};

/* A row's thumbnail slot, so a thumbnail that arrives later can be put into the row showing its key. */
struct RowThumb
{
    lv_obj_t * row;
    lv_obj_t * img;
    lv_obj_t * placeholder;
    std::string key;
};

enum Tab { TAB_QUEUE, TAB_PLAYLISTS };

const uint32_t COLOR_TEXT    = 0xE5E7EB;
const uint32_t COLOR_DIM     = 0x9CA3AF;
const uint32_t COLOR_OFF     = 0x6B7280; /* view-only rows */
const uint32_t COLOR_ACCENT  = 0x60A5FA;
const int32_t HEADER_H       = 48;
const int32_t ROW_H          = 60;
const int32_t THUMB_BOX      = 44;
const size_t THUMB_CACHE_MAX = 48; /* thumbnails kept beyond the ones on screen: 48 x 4.5 KB ~ 220 KB */

lv_obj_t * panel                 = nullptr;
lv_obj_t * header                = nullptr;
lv_obj_t * list                  = nullptr;
browse_cb_t g_cb                 = nullptr;
browse_thumb_req_cb_t g_thumb_cb = nullptr;
std::unordered_map<std::string, Thumb *> thumbs;
std::unordered_set<std::string> thumbs_requested;
std::vector<RowThumb> row_thumbs; /* the rows currently built */

std::vector<Item> queue_items, playlist_items, track_items;
std::string tracks_title;
Tab tab                    = TAB_QUEUE;
bool in_playlist           = false; /* playlists tab shows one playlist's tracks */
lv_timer_t * rebuild_timer = nullptr;

void emit(browse_evt_t evt, int index)
{
    if(g_cb) g_cb(evt, index);
}

std::vector<Item> copy_items(const browse_item_t * items, size_t count)
{
    std::vector<Item> out;
    for(size_t i = 0; items && i < count; i++) {
        Item it;
        it.title    = items[i].title ? items[i].title : "";
        it.subtitle = items[i].subtitle ? items[i].subtitle : "";
        it.playable = items[i].playable;
        it.current  = items[i].current;
        it.cover    = items[i].cover ? items[i].cover : "";
        out.push_back(std::move(it));
    }
    return out;
}

void rebuild();

/* Rows and header buttons delete themselves when the lists are rebuilt, so a tap never rebuilds inline:
 * it asks for a rebuild that runs from a timer, after the event has been handled. */
void schedule_rebuild()
{
    if(rebuild_timer || !panel) return;
    rebuild_timer = lv_timer_create(
        [](lv_timer_t *) {
            rebuild_timer = nullptr; /* the timer deletes itself after its one run */
            rebuild();
        },
        1, nullptr);
    lv_timer_set_repeat_count(rebuild_timer, 1);
}

const lv_font_t * font_title()
{
    return ui_noto_font(20, &lv_font_montserrat_22);
}
const lv_font_t * font_sub()
{
    return ui_noto_font(16, &lv_font_montserrat_16);
}

lv_obj_t * plain_box(lv_obj_t * parent)
{
    lv_obj_t * o = lv_obj_create(parent);
    lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_set_scrollable(o, false);
    return o;
}

void make_tab(const char * text, Tab t)
{
    lv_obj_t * b = plain_box(header);
    lv_obj_set_size(b, LV_SIZE_CONTENT, HEADER_H);
    lv_obj_set_style_pad_hor(b, 14, 0);
    const bool active = (tab == t);
    lv_obj_t * l      = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, font_title(), 0);
    lv_obj_set_style_text_color(l, lv_color_hex(active ? 0xFFFFFF : COLOR_DIM), 0);
    lv_obj_center(l);
    if(active) { /* underline */
        lv_obj_set_style_border_side(b, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_width(b, 3, 0);
        lv_obj_set_style_border_color(b, lv_color_hex(COLOR_ACCENT), 0);
    }
    lv_obj_add_event_cb(
        b,
        [](lv_event_t * e) {
            tab         = (Tab)(intptr_t)lv_event_get_user_data(e);
            in_playlist = false;
            schedule_rebuild();
        },
        LV_EVENT_CLICKED, (void *)(intptr_t)t);
}

void build_header()
{
    lv_obj_clean(header);
    if(tab == TAB_PLAYLISTS && in_playlist) {
        lv_obj_t * back = plain_box(header);
        lv_obj_set_size(back, 56, HEADER_H);
        lv_obj_t * bl = lv_label_create(back);
        lv_label_set_text(bl, LV_SYMBOL_LEFT);
        lv_obj_set_style_text_font(bl, &lv_font_montserrat_22, 0);
        lv_obj_set_style_text_color(bl, lv_color_white(), 0);
        lv_obj_center(bl);
        lv_obj_add_event_cb(
            back,
            [](lv_event_t *) {
                in_playlist = false;
                schedule_rebuild();
            },
            LV_EVENT_CLICKED, nullptr);

        lv_obj_t * t = lv_label_create(header);
        lv_label_set_text(t, tracks_title.c_str());
        lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_flex_grow(t, 1);
        lv_obj_set_style_text_font(t, font_title(), 0);
        lv_obj_set_style_text_color(t, lv_color_white(), 0);
        return;
    }
    make_tab(str::BROWSE_QUEUE, TAB_QUEUE);
    make_tab(str::BROWSE_PLAYLISTS, TAB_PLAYLISTS);
}

void make_row(const Item & it, int index, bool chevron, browse_evt_t evt)
{
    lv_obj_t * row = lv_obj_create(list);
    lv_obj_set_size(row, LV_PCT(100), ROW_H);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollable(row, false);
    lv_obj_set_style_radius(row, 10, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_hor(row, 12, 0);
    lv_obj_set_style_pad_ver(row, 0, 0);
    lv_obj_set_style_pad_column(row, 10, 0);
    lv_obj_set_style_bg_color(row, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(row, it.current ? LV_OPA_20 : LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_10, (lv_style_selector_t)LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(row, 0, 0);

    const uint32_t title_color = it.current ? COLOR_ACCENT : it.playable || chevron ? COLOR_TEXT : COLOR_OFF;

    if(!it.cover.empty()) {
        lv_obj_t * box = plain_box(row);
        lv_obj_set_size(box, THUMB_BOX, THUMB_BOX);
        lv_obj_set_style_radius(box, 6, 0);
        lv_obj_set_style_clip_corner(box, true, 0);
        lv_obj_set_style_bg_color(box, lv_color_hex(0x374151), 0);
        lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
        lv_obj_set_clickable(box, false); /* taps go to the row */
        lv_obj_set_flex_grow(box, 0);

        lv_obj_t * ph = lv_label_create(box);
        lv_label_set_text(ph, LV_SYMBOL_AUDIO);
        lv_obj_set_style_text_color(ph, lv_color_hex(COLOR_OFF), 0);
        lv_obj_center(ph);

        lv_obj_t * img = lv_image_create(box);
        lv_obj_set_size(img, THUMB_BOX, THUMB_BOX);
        lv_image_set_inner_align(img, LV_IMAGE_ALIGN_STRETCH);
        lv_obj_set_clickable(img, false);
        lv_obj_set_hidden(img, true);

        row_thumbs.push_back({row, img, ph, it.cover});
        auto have = thumbs.find(it.cover);
        if(have != thumbs.end()) {
            lv_image_set_src(img, &have->second->dsc);
            lv_obj_set_hidden(img, false);
            lv_obj_set_hidden(ph, true);
        }
    }

    lv_obj_t * text = plain_box(row);
    lv_obj_set_height(text, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(text, 1);
    lv_obj_set_flex_flow(text, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(text, 2, 0);
    lv_obj_set_style_bg_opa(text, LV_OPA_TRANSP, 0);
    lv_obj_set_clickable(text, false); /* taps go to the row */

    lv_obj_t * t = lv_label_create(text);
    lv_label_set_text(t, it.title.c_str());
    lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_size(t, LV_PCT(100),
                    lv_font_get_line_height(font_title())); /* one line: DOTS only cuts a fixed-height label */
    lv_obj_set_style_text_font(t, font_title(), 0);
    lv_obj_set_style_text_color(t, lv_color_hex(title_color), 0);

    if(!it.subtitle.empty()) {
        lv_obj_t * s = lv_label_create(text);
        lv_label_set_text(s, it.subtitle.c_str());
        lv_label_set_long_mode(s, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_size(s, LV_PCT(100), lv_font_get_line_height(font_sub()));
        lv_obj_set_style_text_font(s, font_sub(), 0);
        lv_obj_set_style_text_color(s, lv_color_hex(it.playable || chevron ? COLOR_DIM : COLOR_OFF), 0);
    }

    if(chevron) {
        lv_obj_t * c = lv_label_create(row);
        lv_label_set_text(c, LV_SYMBOL_RIGHT);
        lv_obj_set_style_text_color(c, lv_color_hex(COLOR_DIM), 0);
    } else if(it.current) {
        lv_obj_t * c = lv_label_create(row);
        lv_label_set_text(c, LV_SYMBOL_PLAY);
        lv_obj_set_style_text_color(c, lv_color_hex(COLOR_ACCENT), 0);
    }

    if(chevron || it.playable) {
        lv_obj_add_event_cb(
            row,
            [](lv_event_t * e) {
                /* packed into the user data pointer: low 8 bits = event, the rest = index */
                const intptr_t v = (intptr_t)lv_event_get_user_data(e);
                emit((browse_evt_t)(v & 0xFF), (int)(v >> 8));
            },
            LV_EVENT_CLICKED, (void *)(((intptr_t)index << 8) | (intptr_t)evt));
    } else {
        lv_obj_set_clickable(row, false);
    }
}

/* Ask the provider for the thumbnails of rows in (or one row beyond) the viewport that have none yet. */
void request_visible_thumbs()
{
    if(!g_thumb_cb || !list) return;
    const int32_t top    = lv_obj_get_scroll_y(list);
    const int32_t bottom = top + lv_obj_get_height(list);
    for(const RowThumb & r : row_thumbs) {
        if(thumbs.count(r.key) || thumbs_requested.count(r.key)) continue;
        const int32_t y = lv_obj_get_y(r.row);
        if(y + ROW_H < top - ROW_H || y > bottom + ROW_H) continue;
        thumbs_requested.insert(r.key);
        g_thumb_cb(r.key.c_str());
    }
}

void free_thumb(Thumb * t)
{
    lv_image_cache_drop(&t->dsc);
    free(t->buf);
    delete t;
}

/* Keep the cache small: thumbnails of rows that are built now stay, others go once the cache is over its cap. */
void evict_thumbs()
{
    if(thumbs.size() <= THUMB_CACHE_MAX) return;
    std::unordered_set<std::string> shown;
    for(const RowThumb & r : row_thumbs) shown.insert(r.key);
    for(auto it = thumbs.begin(); it != thumbs.end() && thumbs.size() > THUMB_CACHE_MAX;) {
        if(shown.count(it->first)) {
            ++it;
            continue;
        }
        thumbs_requested.erase(it->first);
        free_thumb(it->second);
        it = thumbs.erase(it);
    }
}

void build_list()
{
    row_thumbs.clear(); /* the rows (and their image widgets) are about to be deleted */
    lv_obj_clean(list);
    lv_obj_scroll_to_y(list, 0, LV_ANIM_OFF);

    const std::vector<Item> * items = &queue_items;
    browse_evt_t evt                = BROWSE_EVT_PLAY_QUEUE;
    bool chevron                    = false;
    if(tab == TAB_PLAYLISTS) {
        items   = in_playlist ? &track_items : &playlist_items;
        evt     = in_playlist ? BROWSE_EVT_PLAY_TRACK : BROWSE_EVT_OPEN_PLAYLIST;
        chevron = !in_playlist;
    }

    if(items->empty()) {
        lv_obj_t * l = lv_label_create(list);
        lv_label_set_text(l, str::BROWSE_EMPTY);
        lv_obj_set_style_text_font(l, font_sub(), 0);
        lv_obj_set_style_text_color(l, lv_color_hex(COLOR_DIM), 0);
        return;
    }
    for(size_t i = 0; i < items->size(); i++) make_row((*items)[i], (int)i, chevron, evt);

    /* start the queue at the playing row so "what is next" is in view */
    if(tab == TAB_QUEUE) {
        for(size_t i = 0; i < items->size(); i++) {
            if((*items)[i].current) {
                lv_obj_update_layout(list);
                lv_obj_scroll_to_y(list, (int32_t)i * (ROW_H + lv_obj_get_style_pad_row(list, LV_PART_MAIN)),
                                   LV_ANIM_OFF);
                break;
            }
        }
    }
}

void rebuild()
{
    if(!panel) return;
    build_header();
    build_list();
    lv_obj_update_layout(list);
    evict_thumbs();
    request_visible_thumbs();
}

} // namespace

lv_obj_t * browse_ui_create(lv_obj_t * parent, int32_t w, int32_t h)
{
    panel = plain_box(parent);
    lv_obj_set_size(panel, w, h);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(panel, 4, 0);

    header = plain_box(panel);
    lv_obj_set_size(header, LV_PCT(100), HEADER_H);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    list = lv_obj_create(panel);
    lv_obj_set_width(list, LV_PCT(100));
    lv_obj_set_flex_grow(list, 1);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 0, 0);
    lv_obj_set_style_pad_row(list, 4, 0);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_event_cb(list, [](lv_event_t *) { request_visible_thumbs(); }, LV_EVENT_SCROLL, nullptr);

    lv_obj_set_hidden(panel, true);
    rebuild();
    return panel;
}

void browse_ui_set_cb(browse_cb_t cb)
{
    g_cb = cb;
}

bool browse_ui_has_cb(void)
{
    return g_cb != nullptr;
}

void browse_ui_opened(void)
{
    tab         = TAB_QUEUE;
    in_playlist = false;
    schedule_rebuild();
    emit(BROWSE_EVT_SHOWN, 0);
}

void browse_ui_set_queue(const browse_item_t * items, size_t count)
{
    queue_items = copy_items(items, count);
    schedule_rebuild();
}

void browse_ui_set_playlists(const browse_item_t * items, size_t count)
{
    playlist_items = copy_items(items, count);
    schedule_rebuild();
}

void browse_ui_set_tracks(const char * playlist_title, const browse_item_t * items, size_t count)
{
    tracks_title = playlist_title ? playlist_title : "";
    track_items  = copy_items(items, count);
    tab          = TAB_PLAYLISTS;
    in_playlist  = true;
    schedule_rebuild();
}

void browse_ui_set_thumb_cb(browse_thumb_req_cb_t cb)
{
    g_thumb_cb = cb;
    request_visible_thumbs();
}

void browse_ui_set_thumb(const char * cover_key, const void * rgb565, uint32_t w, uint32_t h)
{
    if(!cover_key || !rgb565 || w == 0 || h == 0 || thumbs.count(cover_key)) return;
    const size_t size = (size_t)w * h * 2;
    Thumb * t         = new Thumb();
    t->buf            = (unsigned char *)malloc(size);
    if(!t->buf) {
        delete t;
        return;
    }
    memcpy(t->buf, rgb565, size);
    memset(&t->dsc, 0, sizeof(t->dsc));
    t->dsc.header.magic  = LV_IMAGE_HEADER_MAGIC;
    t->dsc.header.cf     = LV_COLOR_FORMAT_RGB565;
    t->dsc.header.w      = w;
    t->dsc.header.h      = h;
    t->dsc.header.stride = w * 2;
    t->dsc.data_size     = (uint32_t)size;
    t->dsc.data          = t->buf;
    thumbs[cover_key]    = t;

    for(const RowThumb & r : row_thumbs) {
        if(r.key != cover_key) continue;
        lv_image_set_src(r.img, &t->dsc);
        lv_obj_set_hidden(r.img, false);
        lv_obj_set_hidden(r.placeholder, true);
    }
}
