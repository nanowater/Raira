#include "settings_ui.h"
#include <cstring>
#include <vector>
#include "media_poller.h"
#include "strings.h"
#include "ui_fonts.h"

namespace {

enum Page {
    PAGE_MAIN,
    PAGE_WIFI,
    PAGE_PASSWORD,
    PAGE_SERVER,
    PAGE_TOKEN,
    PAGE_PLAYER,
    PAGE_SOURCES,
    PAGE_MENU,
    PAGE_COUNT
};

const uint32_t COLOR_BG          = 0x12151B;
const uint32_t COLOR_ROW         = 0x1F2937;
const uint32_t COLOR_ROW_PRESSED = 0x374151;
const uint32_t COLOR_DIM         = 0x9CA3AF;
const uint32_t COLOR_ACCENT      = 0x60A5FA;
const uint32_t COLOR_GOOD        = 0x34D399;
const uint32_t COLOR_BAD         = 0xF87171;

SettingsServices svc;
lv_obj_t * root              = nullptr;
lv_obj_t * title_lbl         = nullptr;
lv_obj_t * pages[PAGE_COUNT] = {};
int page_now                 = PAGE_MAIN;
lv_timer_t * poll_timer      = nullptr;

/* main page */
lv_obj_t * wifi_value   = nullptr;
lv_obj_t * server_value = nullptr;
lv_obj_t * token_value  = nullptr;

/* menu page (long press on the cover): where back goes from the pages it leads to */
lv_obj_t * menu_player_value = nullptr;
bool settings_from_menu      = false; /* the settings page was entered from the menu: back returns there */
bool player_from_menu        = false; /* so was the player page */

/* player page: the players the service reported last, the row for the current choice is highlighted */
lv_obj_t * player_value  = nullptr;
lv_obj_t * player_status = nullptr;
lv_obj_t * player_list   = nullptr;
std::vector<PlayerInfo> player_infos;

/* sources page (hub): pairing code and the list of sources */
lv_obj_t * sources_value   = nullptr;
lv_obj_t * sources_btn_lbl = nullptr;
lv_obj_t * sources_code    = nullptr;
lv_obj_t * sources_secs    = nullptr;
lv_obj_t * sources_list    = nullptr;
std::string sources_sig; /* what the list shows now: rebuilt only when this changes */

/* wifi page */
lv_obj_t * wifi_status = nullptr;
lv_obj_t * wifi_list   = nullptr;
bool scan_was_running  = false;
std::string selected_ssid;    /* network being configured */
std::string pending_password; /* password of the connection attempt in progress */
bool wait_for_connect = false;

/* password / server pages share one keyboard */
lv_obj_t * keyboard     = nullptr;
lv_obj_t * pw_ssid_lbl  = nullptr;
lv_obj_t * pw_input     = nullptr;
lv_obj_t * pw_eye_lbl   = nullptr;
lv_obj_t * server_input = nullptr;
lv_obj_t * server_msg   = nullptr;
lv_obj_t * token_input  = nullptr;
lv_obj_t * token_msg    = nullptr;

const lv_font_t * f(int px, bool bold = false)
{
    return ui_noto_font(px, px >= 28 ? &lv_font_montserrat_28 : &lv_font_montserrat_22, bold);
}

void show_page(int p);

/* One row of the network list; freed when the row is deleted. */
struct NetRow
{
    std::string ssid;
    bool secured;
};

void on_network_clicked(lv_event_t * e)
{
    const NetRow * nr = static_cast<const NetRow *>(lv_event_get_user_data(e));
    selected_ssid     = nr->ssid;
    if(nr->secured) {
        lv_label_set_text(pw_ssid_lbl, selected_ssid.c_str());
        lv_textarea_set_text(pw_input, "");
        lv_textarea_set_password_mode(pw_input, true);
        lv_label_set_text(pw_eye_lbl, LV_SYMBOL_EYE_CLOSE);
        show_page(PAGE_PASSWORD);
    } else if(svc.wifi) {
        pending_password.clear();
        svc.wifi->connect(selected_ssid, "");
        wait_for_connect = true;
    }
}

lv_obj_t * make_label(lv_obj_t * parent, const char * text, int px, uint32_t color)
{
    lv_obj_t * l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, f(px), 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    return l;
}

/* Rounded dark button with a text label. */
lv_obj_t * make_button(lv_obj_t * parent, const char * text, int w, int h, uint32_t bg, lv_event_cb_t cb,
                       void * user = nullptr)
{
    lv_obj_t * b = lv_button_create(parent);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_radius(b, 12, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(COLOR_ROW_PRESSED), LV_STATE_PRESSED);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, user);
    lv_obj_t * l = make_label(b, text, 22, 0xFFFFFF);
    lv_obj_center(l);
    return b;
}

/* A square tile: big symbol, name below, an optional small value line under the name (returned through value_out). x =
 * -1 leaves the position to the parent (a flex container); otherwise (x, y) is the top-left corner. */
lv_obj_t * make_tile(lv_obj_t * parent, int x, int y, int size, const char * symbol, const char * title,
                     lv_obj_t ** value_out, lv_event_cb_t cb)
{
    lv_obj_t * t = lv_obj_create(parent);
    lv_obj_set_size(t, size, size);
    if(x >= 0) lv_obj_set_pos(t, x, y);
    lv_obj_set_style_radius(t, 28, 0);
    lv_obj_set_style_bg_color(t, lv_color_hex(COLOR_ROW), 0);
    lv_obj_set_style_bg_opa(t, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(t, lv_color_hex(COLOR_ROW_PRESSED), LV_STATE_PRESSED);
    lv_obj_set_style_border_width(t, 0, 0);
    lv_obj_set_style_pad_all(t, 12, 0);
    lv_obj_set_scrollable(t, false);
    if(cb) lv_obj_add_event_cb(t, cb, LV_EVENT_CLICKED, nullptr);
    /* icon, name and value line form one group, centered in the tile as a whole */
    lv_obj_t * group = lv_obj_create(t);
    lv_obj_set_size(group, size - 24, LV_SIZE_CONTENT);
    lv_obj_center(group);
    lv_obj_set_style_bg_opa(group, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(group, 0, 0);
    lv_obj_set_style_pad_all(group, 0, 0);
    lv_obj_set_style_pad_row(group, 6, 0);
    lv_obj_set_flex_flow(group, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(group, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollable(group, false);
    lv_obj_set_clickable(group, false); /* a tap goes to the tile */
    lv_obj_t * icon = make_label(group, symbol, 28, COLOR_ACCENT);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_48, 0); /* the symbols live only in the built-in font */
    lv_obj_t * name = make_label(group, title, 26, 0xFFFFFF);
    /* one line: a longer name is cut with "..." instead of wrapping over the icon; the height is the font's line
     * height, a smaller one cuts the descenders (g, p, y) */
    lv_obj_set_size(name, size - 24, lv_font_get_line_height(f(26)) + 2);
    lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    if(value_out) {
        *value_out = make_label(group, "", 18, COLOR_DIM);
        lv_obj_set_size(*value_out, size - 24, lv_font_get_line_height(f(18)) + 2);
        lv_obj_set_style_text_align(*value_out, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(*value_out, LV_LABEL_LONG_DOT);
    }
    return t;
}

struct TileSpec
{
    const char * symbol;
    const char * title;
    lv_obj_t ** value_out; /* nullptr: no value line */
    lv_event_cb_t cb;
};

/* Lay the tiles of a menu page out as big as the page allows: up to three in one row (at most 300 px), four to six in
 * two rows (190 px); every row is centered, the whole block sits in the middle of the page. */
void layout_tiles(lv_obj_t * page, const std::vector<TileSpec> & specs)
{
    const int n = static_cast<int>(specs.size());
    if(n == 0) return;
    const int gap    = 24;
    const int page_h = 480 - 64;
    const int cols   = n <= 3 ? n : (n + 1) / 2;
    const int rows   = (n + cols - 1) / cols;
    const int size   = n <= 3 ? std::min(300, (740 - (n - 1) * gap) / n) : 190;
    const int top    = (page_h - (rows * size + (rows - 1) * gap)) / 2;
    for(int i = 0; i < n; i++) {
        const int row    = i / cols;
        const int in_row = std::min(cols, n - row * cols);
        const int left   = (800 - (in_row * size + (in_row - 1) * gap)) / 2;
        make_tile(page, left + (i % cols) * (size + gap), top + row * (size + gap), size, specs[i].symbol,
                  specs[i].title, specs[i].value_out, specs[i].cb);
    }
}

void set_status(lv_obj_t * lbl, const std::string & text, uint32_t color)
{
    lv_label_set_text(lbl, text.c_str());
    lv_obj_set_style_text_color(lbl, lv_color_hex(color), 0);
}

const char * page_title(int p)
{
    switch(p) {
        case PAGE_WIFI: return str::WIFI;
        case PAGE_PASSWORD: return str::PASSWORD;
        case PAGE_SERVER: return str::SERVER_TITLE;
        case PAGE_TOKEN: return str::TOKEN_TITLE;
        case PAGE_PLAYER: return str::PLAYER_TITLE;
        case PAGE_SOURCES: return str::SOURCES_TITLE;
        case PAGE_MENU: return str::MENU_TITLE;
        default: return str::SETTINGS;
    }
}

/* The current player choice as shown in a row: "Automatic", or the name of the chosen player. */
std::string player_text()
{
    const std::string id = svc.get_player ? svc.get_player() : "";
    std::string text     = id.empty() ? str::PLAYER_AUTO : id;
    for(const PlayerInfo & p : player_infos)
        if(!id.empty() && p.id == id) text = p.name;
    return text;
}

void refresh_main()
{
    std::string wifi_text = str::NOT_CONNECTED;
    if(svc.wifi) {
        switch(svc.wifi->state()) {
            case WifiState::Connected: wifi_text = svc.wifi->connected_ssid(); break;
            case WifiState::Connecting: wifi_text = str::CONNECTING; break;
            case WifiState::Failed: wifi_text = str::CONNECT_FAILED; break;
            default: break;
        }
    }
    lv_label_set_text(wifi_value, wifi_text.c_str());
    std::string server = svc.get_server ? svc.get_server() : "";
    lv_label_set_text(server_value, server.empty() ? str::NOT_SET : server.c_str());
    if(sources_value && svc.hub) {
        int connected = 0, paired = 0;
        for(const HubSourceInfo & s : svc.hub->sources()) {
            connected += s.connected && s.paired;
            paired += s.paired;
        }
        std::string text =
            paired == 0 ? str::NOT_SET : std::to_string(connected) + " / " + std::to_string(paired) + " connected";
        lv_label_set_text(sources_value, text.c_str());
    }
    if(player_value) lv_label_set_text(player_value, player_text().c_str());
    if(token_value) {
        const bool has_token = svc.get_token && !svc.get_token().empty();
        lv_label_set_text(token_value, has_token ? "********" : str::NOT_SET); /* never show the token itself */
    }
}

void show_page(int p)
{
    page_now = p;
    for(int i = 0; i < PAGE_COUNT; i++) lv_obj_set_hidden(pages[i], i != p);
    lv_label_set_text(title_lbl, page_title(p));
    const bool typing = (p == PAGE_PASSWORD || p == PAGE_SERVER || p == PAGE_TOKEN);
    lv_obj_set_hidden(keyboard, !typing);
    if(p == PAGE_PASSWORD) lv_keyboard_set_textarea(keyboard, pw_input);
    if(p == PAGE_SERVER) lv_keyboard_set_textarea(keyboard, server_input);
    if(p == PAGE_TOKEN) lv_keyboard_set_textarea(keyboard, token_input);
    if(p == PAGE_MAIN) refresh_main();
    if(p == PAGE_MENU && menu_player_value) lv_label_set_text(menu_player_value, player_text().c_str());
}

void fill_wifi_list()
{
    lv_obj_clean(wifi_list);
    if(!svc.wifi) return;
    const std::string current = svc.wifi->connected_ssid();
    for(const WifiNetwork & n : svc.wifi->scan_result()) {
        lv_obj_t * r = lv_obj_create(wifi_list);
        lv_obj_set_size(r, lv_pct(100), 60);
        lv_obj_set_style_radius(r, 12, 0);
        lv_obj_set_style_bg_color(r, lv_color_hex(COLOR_ROW), 0);
        lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(r, lv_color_hex(COLOR_ROW_PRESSED), LV_STATE_PRESSED);
        lv_obj_set_style_border_width(r, 0, 0);
        lv_obj_set_style_pad_hor(r, 20, 0);
        lv_obj_set_scrollable(r, false);

        auto * nr = new NetRow{n.ssid, n.secured}; /* travels with the row, freed on delete */
        lv_obj_add_event_cb(r, on_network_clicked, LV_EVENT_CLICKED, nr);
        lv_obj_add_event_cb(
            r, [](lv_event_t * e) { delete static_cast<NetRow *>(lv_event_get_user_data(e)); }, LV_EVENT_DELETE, nr);

        lv_obj_t * name = make_label(r, n.ssid.c_str(), 22, 0xFFFFFF);
        lv_obj_align(name, LV_ALIGN_LEFT_MID, 44, 0);
        lv_obj_t * sig =
            make_label(r, LV_SYMBOL_WIFI, 22, n.rssi > -60 ? COLOR_GOOD : (n.rssi > -75 ? COLOR_ACCENT : COLOR_DIM));
        lv_obj_align(sig, LV_ALIGN_LEFT_MID, 0, 0);
        lv_obj_t * tag = make_label(
            r, n.secured ? str::TAG_SECURED : (n.ssid == current ? str::TAG_CONNECTED : str::TAG_OPEN), 16, COLOR_DIM);
        lv_obj_align(tag, LV_ALIGN_RIGHT_MID, 0, 0);
        if(n.ssid == current) {
            lv_obj_set_style_border_width(r, 2, 0);
            lv_obj_set_style_border_color(r, lv_color_hex(COLOR_GOOD), 0);
        }
    }
}

void fill_player_list();

/* One tile of the player list: "Automatic" (id "") or a player of the service; the chosen one has a green border. */
void add_player_row(const std::string & id, const std::string & name, const std::string & tag, bool selected)
{
    lv_obj_t * tag_lbl = nullptr;
    lv_obj_t * r = make_tile(player_list, -1, 0, 200, id.empty() ? LV_SYMBOL_REFRESH : LV_SYMBOL_AUDIO, name.c_str(),
                             &tag_lbl, nullptr);
    lv_label_set_text(tag_lbl, tag.c_str());
    lv_obj_set_style_text_font(tag_lbl, f(16), 0);
    auto * idp = new std::string(id); /* travels with the tile, freed on delete */
    lv_obj_add_event_cb(
        r,
        [](lv_event_t * e) {
            if(svc.set_player) svc.set_player(*static_cast<std::string *>(lv_event_get_user_data(e)));
            /* the tiles are rebuilt (to move the highlight) after this click has been handled */
            lv_async_call(
                [](void *) {
                    if(root && page_now == PAGE_PLAYER) fill_player_list();
                },
                nullptr);
        },
        LV_EVENT_CLICKED, idp);
    lv_obj_add_event_cb(
        r, [](lv_event_t * e) { delete static_cast<std::string *>(lv_event_get_user_data(e)); }, LV_EVENT_DELETE, idp);
    if(selected) {
        lv_obj_set_style_border_width(r, 3, 0);
        lv_obj_set_style_border_color(r, lv_color_hex(COLOR_GOOD), 0);
    }
}
void fill_player_list()
{
    lv_obj_clean(player_list);
    const std::string cur = svc.get_player ? svc.get_player() : "";
    add_player_row("", str::PLAYER_AUTO, str::PLAYER_AUTO_SUB, cur.empty());
    for(const PlayerInfo & p : player_infos) add_player_row(p.id, p.name, p.service + " - " + p.status, p.id == cur);
}

void on_open_player(lv_event_t *)
{
    player_from_menu = (page_now == PAGE_MENU);
    lv_obj_clean(player_list);
    set_status(player_status, str::PLAYER_LOADING, COLOR_DIM);
    show_page(PAGE_PLAYER);
    if(!svc.list_players) return;
    svc.list_players([](bool ok, const std::vector<PlayerInfo> & players, const std::string &) {
        if(!root) return;
        if(!ok) {
            set_status(player_status, str::PLAYER_FAILED, COLOR_BAD);
            return;
        }
        player_infos = players;
        set_status(player_status, players.empty() ? str::PLAYER_NONE : "", COLOR_DIM);
        if(page_now == PAGE_PLAYER) fill_player_list();
    });
}

/* ---- sources page ---- */

void add_source_row(const HubSourceInfo & s)
{
    lv_obj_t * r = lv_obj_create(sources_list);
    lv_obj_set_size(r, lv_pct(100), 60);
    lv_obj_set_style_radius(r, 12, 0);
    lv_obj_set_style_bg_color(r, lv_color_hex(COLOR_ROW), 0);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(r, 0, 0);
    lv_obj_set_style_pad_hor(r, 20, 0);
    lv_obj_set_scrollable(r, false);

    std::string label = s.name.empty() ? s.id : s.name;
    if(!s.os.empty()) label += "  (" + s.os + ")";
    lv_obj_t * n = make_label(r, label.c_str(), 22, 0xFFFFFF);
    lv_obj_align(n, LV_ALIGN_LEFT_MID, 0, 0);

    const char * tag     = !s.paired ? str::SOURCES_WAITING : s.connected ? str::TAG_CONNECTED : str::NOT_CONNECTED;
    const uint32_t color = !s.paired ? COLOR_ACCENT : s.connected ? COLOR_GOOD : COLOR_DIM;
    lv_obj_t * t         = make_label(r, tag, 18, color);
    lv_obj_align(t, LV_ALIGN_RIGHT_MID, s.paired ? -120 : 0, 0);
    if(s.paired) {
        auto * id     = new std::string(s.id);
        lv_obj_t * rm = make_button(
            r, str::SOURCES_REMOVE, 108, 44, 0x7F1D1D,
            [](lv_event_t * e) {
                if(svc.hub) svc.hub->remove_source(*static_cast<std::string *>(lv_event_get_user_data(e)));
                sources_sig.clear(); /* the list is rebuilt on the next tick, after this click is handled */
            },
            id);
        lv_obj_add_event_cb(
            rm, [](lv_event_t * e) { delete static_cast<std::string *>(lv_event_get_user_data(e)); }, LV_EVENT_DELETE,
            id);
        lv_obj_align(rm, LV_ALIGN_RIGHT_MID, 0, 0);
    }
}

void refresh_sources()
{
    if(!svc.hub || !sources_list) return;
    const std::string code = svc.hub->pairing_code();
    lv_label_set_text(sources_btn_lbl, code.empty() ? str::SOURCES_PAIR : str::SOURCES_CANCEL);
    if(code.empty()) {
        lv_label_set_text(sources_code, "");
        lv_label_set_text(sources_secs, "");
    } else {
        lv_label_set_text(sources_code, (code.substr(0, 3) + " " + code.substr(3)).c_str()); /* 123 456 */
        const int left = svc.hub->pairing_seconds_left();
        char buf[16];
        snprintf(buf, sizeof(buf), "%d:%02d", left / 60, left % 60);
        lv_label_set_text(sources_secs, buf);
    }

    const std::vector<HubSourceInfo> list = svc.hub->sources();
    std::string sig;
    for(const HubSourceInfo & s : list)
        sig += s.id + "|" + s.name + "|" + s.os + (s.paired ? "P" : "-") + (s.connected ? "C" : "-") + ";";
    if(sig == sources_sig) return;
    sources_sig = sig;
    lv_obj_clean(sources_list);
    if(list.empty()) make_label(sources_list, str::SOURCES_NONE, 20, COLOR_DIM);
    for(const HubSourceInfo & s : list) add_source_row(s);
}

void on_toggle_pairing(lv_event_t *)
{
    if(!svc.hub) return;
    if(svc.hub->pairing_code().empty())
        svc.hub->start_pairing();
    else
        svc.hub->stop_pairing();
    refresh_sources();
}

void on_open_sources(lv_event_t *)
{
    sources_sig.clear();
    show_page(PAGE_SOURCES);
    refresh_sources();
}

void start_scan()
{
    if(!svc.wifi) return;
    svc.wifi->start_scan();
    scan_was_running = true;
    lv_obj_clean(wifi_list);
    set_status(wifi_status, str::SCANNING, COLOR_DIM);
}

void refresh_sources();

void poll_cb(lv_timer_t *)
{
    if(page_now == PAGE_SOURCES) refresh_sources(); /* the code counts down and sources come and go */
    if(!svc.wifi) return;
    if(page_now == PAGE_WIFI && scan_was_running && !svc.wifi->scan_running()) {
        scan_was_running = false;
        fill_wifi_list();
        set_status(wifi_status,
                   svc.wifi->state() == WifiState::Connected
                       ? (std::string(str::CONNECTED_TO) + svc.wifi->connected_ssid())
                       : str::SELECT_NETWORK,
                   svc.wifi->state() == WifiState::Connected ? COLOR_GOOD : COLOR_DIM);
    }
    if(wait_for_connect) {
        switch(svc.wifi->state()) {
            case WifiState::Connecting:
                set_status(wifi_status, std::string(str::CONNECTING_TO) + selected_ssid + " ...", COLOR_ACCENT);
                break;
            case WifiState::Connected:
                wait_for_connect = false;
                set_status(wifi_status, std::string(str::CONNECTED_TO) + svc.wifi->connected_ssid(), COLOR_GOOD);
                if(svc.remember_wifi) svc.remember_wifi(selected_ssid, pending_password);
                if(page_now == PAGE_WIFI) fill_wifi_list();
                break;
            case WifiState::Failed:
                wait_for_connect = false;
                set_status(wifi_status, str::CHECK_PASSWORD, COLOR_BAD);
                break;
            default: break;
        }
    }
    if(page_now == PAGE_MAIN) refresh_main();
}

void close_overlay()
{
    lv_obj_set_hidden(root, true);
    if(poll_timer) lv_timer_pause(poll_timer);
}

void on_back(lv_event_t *)
{
    switch(page_now) {
        case PAGE_PASSWORD: show_page(PAGE_WIFI); break;
        case PAGE_WIFI:
        case PAGE_SERVER:
        case PAGE_TOKEN:
        case PAGE_SOURCES: show_page(PAGE_MAIN); break;
        case PAGE_PLAYER: show_page(player_from_menu ? PAGE_MENU : PAGE_MAIN); break;
        case PAGE_MAIN:
            if(settings_from_menu)
                show_page(PAGE_MENU);
            else
                close_overlay();
            break;
        default: close_overlay(); break; /* the menu */
    }
}

void on_open_settings_from_menu(lv_event_t *)
{
    settings_from_menu = true;
    show_page(PAGE_MAIN);
}

void on_open_wifi(lv_event_t *)
{
    show_page(PAGE_WIFI);
    if(svc.wifi->state() == WifiState::Connected)
        set_status(wifi_status, std::string(str::CONNECTED_TO) + svc.wifi->connected_ssid(), COLOR_GOOD);
    start_scan();
}

void on_open_server(lv_event_t *)
{
    lv_textarea_set_text(server_input, svc.get_server ? svc.get_server().c_str() : "");
    set_status(server_msg, "", COLOR_DIM);
    show_page(PAGE_SERVER);
}

void on_open_token(lv_event_t *)
{
    lv_textarea_set_text(token_input, svc.get_token ? svc.get_token().c_str() : "");
    set_status(token_msg, "", COLOR_DIM);
    show_page(PAGE_TOKEN);
}

void on_rescan(lv_event_t *)
{
    start_scan();
}

void connect_with_password()
{
    if(!svc.wifi) return;
    pending_password = lv_textarea_get_text(pw_input);
    svc.wifi->connect(selected_ssid, pending_password);
    wait_for_connect = true;
    set_status(wifi_status, std::string(str::CONNECTING_TO) + selected_ssid + " ...", COLOR_ACCENT);
    show_page(PAGE_WIFI);
}

void on_connect(lv_event_t *)
{
    connect_with_password();
}

void on_toggle_eye(lv_event_t *)
{
    bool hidden = lv_textarea_get_password_mode(pw_input);
    lv_textarea_set_password_mode(pw_input, !hidden);
    lv_label_set_text(pw_eye_lbl, hidden ? LV_SYMBOL_EYE_OPEN : LV_SYMBOL_EYE_CLOSE);
}

void save_server()
{
    std::string text = lv_textarea_get_text(server_input);
    if(svc.set_server && svc.set_server(text)) {
        /* Stay on this page: the poller now switches to the new address and on_connection_state()
         * shows "checking..." followed by the connected/not-connected result right here. */
    } else {
        set_status(server_msg, str::SERVER_INVALID, COLOR_BAD);
    }
}

void on_save_server(lv_event_t *)
{
    save_server();
}

void save_token()
{
    if(svc.set_token)
        svc.set_token(lv_textarea_get_text(token_input)); /* the result appears via on_connection_state() */
}

void on_save_token(lv_event_t *)
{
    save_token();
}

/* Connection state changes reported by the poller (LVGL thread). UNKNOWN is the moment right after
 * the endpoint was switched; the LOST/CONNECTED changes that follow are its polls' verdict. */
void on_connection_state(media_conn_state_t state)
{
    if(!root) return;
    lv_obj_t * msg = page_now == PAGE_TOKEN ? token_msg : server_msg; /* the page being edited shows the verdict */
    switch(state) {
        case MEDIA_CONN_UNKNOWN: set_status(msg, str::SERVER_CHECKING, COLOR_DIM); break;
        case MEDIA_CONN_CONNECTED: set_status(msg, str::SERVER_CONNECTED, COLOR_GOOD); break;
        case MEDIA_CONN_LOST: set_status(msg, str::SERVER_UNREACHABLE, COLOR_BAD); break;
    }
}

/* Enter key on the keyboard */
void on_keyboard_ready(lv_event_t *)
{
    if(page_now == PAGE_PASSWORD)
        connect_with_password();
    else if(page_now == PAGE_SERVER)
        save_server();
    else if(page_now == PAGE_TOKEN)
        save_token();
}

lv_obj_t * make_page(lv_obj_t * parent)
{
    lv_obj_t * p = lv_obj_create(parent);
    lv_obj_set_size(p, 800, 480 - 64);
    lv_obj_set_pos(p, 0, 64);
    lv_obj_set_style_bg_opa(p, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(p, 0, 0);
    lv_obj_set_style_pad_all(p, 0, 0);
    lv_obj_set_scrollable(p, false);
    return p;
}

lv_obj_t * make_textarea(lv_obj_t * parent, int x, int y, int w)
{
    lv_obj_t * ta = lv_textarea_create(parent);
    lv_obj_set_size(ta, w, 56);
    lv_obj_set_pos(ta, x, y);
    lv_textarea_set_one_line(ta, true);
    lv_obj_set_style_bg_color(ta, lv_color_hex(COLOR_ROW), 0);
    lv_obj_set_style_bg_opa(ta, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(ta, 0, 0);
    lv_obj_set_style_radius(ta, 12, 0);
    lv_obj_set_style_text_font(ta, &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_color(ta, lv_color_white(), 0);
    lv_obj_set_style_pad_hor(ta, 18, 0);
    return ta;
}

} // namespace

void settings_ui_init(SettingsServices services)
{
    svc               = std::move(services);
    lv_obj_t * screen = lv_screen_active();

    root = lv_obj_create(screen);
    lv_obj_set_size(root, 800, 480);
    lv_obj_set_pos(root, 0, 0);
    lv_obj_set_style_bg_color(root, lv_color_hex(COLOR_BG), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_style_radius(root, 0, 0);
    lv_obj_set_style_pad_all(root, 0, 0);
    lv_obj_set_scrollable(root, false);

    /* header */
    lv_obj_t * back = lv_button_create(root);
    lv_obj_set_size(back, 56, 56);
    lv_obj_set_pos(back, 12, 4);
    lv_obj_set_style_bg_opa(back, LV_OPA_TRANSP, 0);
    lv_obj_set_style_shadow_width(back, 0, 0);
    lv_obj_set_style_bg_opa(back, LV_OPA_20, LV_STATE_PRESSED);
    lv_obj_add_event_cb(back, on_back, LV_EVENT_CLICKED, nullptr);
    lv_obj_t * back_lbl = make_label(back, LV_SYMBOL_LEFT, 28, 0xFFFFFF);
    lv_obj_center(back_lbl);
    title_lbl = make_label(root, str::SETTINGS, 28, 0xFFFFFF);
    lv_obj_set_pos(title_lbl, 84, 12);

    for(int i = 0; i < PAGE_COUNT; i++) pages[i] = make_page(root);

    /* menu page: what the long press on the cover leads to */
    {
        std::vector<TileSpec> tiles;
        if(svc.get_player) tiles.push_back({LV_SYMBOL_AUDIO, str::PLAYER_ROW, &menu_player_value, on_open_player});
        tiles.push_back({LV_SYMBOL_SETTINGS, str::SETTINGS, nullptr, on_open_settings_from_menu});
        layout_tiles(pages[PAGE_MENU], tiles);
    }

    /* main page */
    /* tiles; which ones exist depends on what the music service needs (tidal-hifi: server; bridge: server + token +
     * player; hub: player + sources) */
    std::vector<TileSpec> tiles;
    if(svc.wifi)
        tiles.push_back({LV_SYMBOL_WIFI, str::WIFI, &wifi_value, on_open_wifi});
    else
        wifi_value = lv_label_create(pages[PAGE_MAIN]);
    if(!svc.hub) {
        tiles.push_back({LV_SYMBOL_DRIVE, str::SERVER_ROW, &server_value, on_open_server});
    } else {
        server_value = lv_label_create(pages[PAGE_MAIN]); /* refresh_main() writes to it; the hub has no address */
        lv_obj_set_hidden(server_value, true);
    }
    if(svc.get_token) tiles.push_back({LV_SYMBOL_EYE_CLOSE, str::TOKEN_ROW, &token_value, on_open_token});
    if(svc.get_player) tiles.push_back({LV_SYMBOL_AUDIO, str::PLAYER_ROW, &player_value, on_open_player});
    if(svc.hub) tiles.push_back({LV_SYMBOL_USB, str::SOURCES_ROW, &sources_value, on_open_sources});
    layout_tiles(pages[PAGE_MAIN], tiles);

    /* wifi page */
    wifi_status = make_label(pages[PAGE_WIFI], "", 22, COLOR_DIM);
    lv_obj_set_pos(wifi_status, 30, 18);
    lv_obj_t * rescan = make_button(pages[PAGE_WIFI], str::RESCAN, 150, 46, COLOR_ROW, on_rescan);
    lv_obj_set_pos(rescan, 620, 8);
    wifi_list = lv_obj_create(pages[PAGE_WIFI]);
    lv_obj_set_size(wifi_list, 740, 480 - 64 - 70);
    lv_obj_set_pos(wifi_list, 30, 64);
    lv_obj_set_style_bg_opa(wifi_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(wifi_list, 0, 0);
    lv_obj_set_style_pad_all(wifi_list, 0, 0);
    lv_obj_set_style_pad_row(wifi_list, 10, 0);
    lv_obj_set_flex_flow(wifi_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollbar_mode(wifi_list, LV_SCROLLBAR_MODE_OFF);

    /* sources page */
    if(svc.hub) {
        lv_obj_t * pair = make_button(pages[PAGE_SOURCES], str::SOURCES_PAIR, 250, 56, 0x2563EB, on_toggle_pairing);
        lv_obj_set_pos(pair, 30, 12);
        sources_btn_lbl = lv_obj_get_child(pair, 0);
        sources_code    = make_label(pages[PAGE_SOURCES], "", 28, 0xFFFFFF);
        lv_obj_set_style_text_font(sources_code, &lv_font_montserrat_44, 0);
        lv_obj_set_pos(sources_code, 310, 8);
        sources_secs = make_label(pages[PAGE_SOURCES], "", 22, COLOR_DIM);
        lv_obj_set_pos(sources_secs, 590, 24);
        lv_obj_t * hint = make_label(pages[PAGE_SOURCES], str::SOURCES_HINT, 18, COLOR_DIM);
        lv_obj_set_pos(hint, 30, 76);
        sources_list = lv_obj_create(pages[PAGE_SOURCES]);
        lv_obj_set_size(sources_list, 740, 416 - 112 - 8);
        lv_obj_set_pos(sources_list, 30, 112);
        lv_obj_set_style_bg_opa(sources_list, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(sources_list, 0, 0);
        lv_obj_set_style_pad_all(sources_list, 0, 0);
        lv_obj_set_style_pad_row(sources_list, 10, 0);
        lv_obj_set_flex_flow(sources_list, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_scrollbar_mode(sources_list, LV_SCROLLBAR_MODE_OFF);
    }

    /* player page */
    player_status = make_label(pages[PAGE_PLAYER], "", 22, COLOR_DIM);
    lv_obj_set_pos(player_status, 30, 18);
    player_list = lv_obj_create(pages[PAGE_PLAYER]);
    lv_obj_set_size(player_list, 740, 480 - 64 - 70);
    lv_obj_set_pos(player_list, 30, 64);
    lv_obj_set_style_bg_opa(player_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(player_list, 0, 0);
    lv_obj_set_style_pad_all(player_list, 0, 0);
    lv_obj_set_style_pad_row(player_list, 20, 0);
    lv_obj_set_style_pad_column(player_list, 20, 0);
    lv_obj_set_flex_flow(player_list, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_scrollbar_mode(player_list, LV_SCROLLBAR_MODE_OFF);

    /* password page */
    pw_ssid_lbl = make_label(pages[PAGE_PASSWORD], "", 26, COLOR_ACCENT);
    lv_obj_set_pos(pw_ssid_lbl, 30, 12);
    pw_input = make_textarea(pages[PAGE_PASSWORD], 30, 56, 540);
    lv_textarea_set_placeholder_text(pw_input, str::PASSWORD);
    lv_textarea_set_password_mode(pw_input, true);
    lv_obj_t * eye = lv_button_create(pages[PAGE_PASSWORD]);
    lv_obj_set_size(eye, 56, 56);
    lv_obj_set_pos(eye, 582, 56);
    lv_obj_set_style_radius(eye, 12, 0);
    lv_obj_set_style_bg_color(eye, lv_color_hex(COLOR_ROW), 0);
    lv_obj_set_style_shadow_width(eye, 0, 0);
    lv_obj_add_event_cb(eye, on_toggle_eye, LV_EVENT_CLICKED, nullptr);
    pw_eye_lbl = make_label(eye, LV_SYMBOL_EYE_CLOSE, 22, 0xFFFFFF);
    lv_obj_center(pw_eye_lbl);
    lv_obj_t * connect = make_button(pages[PAGE_PASSWORD], str::CONNECT, 140, 56, 0x2563EB, on_connect);
    lv_obj_set_pos(connect, 650, 56);

    /* server page */
    lv_obj_t * hint = make_label(pages[PAGE_SERVER], str::SERVER_HINT, 18, COLOR_DIM);
    lv_obj_set_pos(hint, 30, 12);
    server_input = make_textarea(pages[PAGE_SERVER], 30, 48, 600);
    lv_textarea_set_placeholder_text(server_input, str::SERVER_PLACEHOLDER);
    lv_obj_t * save = make_button(pages[PAGE_SERVER], str::SAVE, 140, 56, 0x2563EB, on_save_server);
    lv_obj_set_pos(save, 650, 48);
    server_msg = make_label(pages[PAGE_SERVER], "", 18, COLOR_BAD);
    lv_obj_set_pos(server_msg, 30, 112);

    /* token page */
    lv_obj_t * token_hint = make_label(pages[PAGE_TOKEN], str::TOKEN_HINT, 18, COLOR_DIM);
    lv_obj_set_pos(token_hint, 30, 12);
    token_input = make_textarea(pages[PAGE_TOKEN], 30, 48, 600);
    lv_textarea_set_placeholder_text(token_input, str::TOKEN_PLACEHOLDER);
    lv_obj_t * token_save = make_button(pages[PAGE_TOKEN], str::SAVE, 140, 56, 0x2563EB, on_save_token);
    lv_obj_set_pos(token_save, 650, 48);
    token_msg = make_label(pages[PAGE_TOKEN], "", 18, COLOR_BAD);
    lv_obj_set_pos(token_msg, 30, 112);

    /* shared keyboard along the bottom */
    keyboard = lv_keyboard_create(root);
    lv_obj_set_size(keyboard, 800, 250);
    lv_obj_align(keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_event_cb(keyboard, on_keyboard_ready, LV_EVENT_READY, nullptr);

    poll_timer = lv_timer_create(poll_cb, 200, nullptr);
    lv_timer_pause(poll_timer);
    media_poller_set_connection_cb(on_connection_state); /* save results appear on the server page */
    show_page(PAGE_MAIN);
    lv_obj_set_hidden(root, true);
}

static void show_overlay(int page)
{
    if(!root) return;
    settings_from_menu = false; /* set again when the settings page is entered from the menu */
    player_from_menu   = false;
    show_page(page);
    lv_obj_set_hidden(root, false);
    lv_obj_move_foreground(root);
    lv_timer_resume(poll_timer);
}

void settings_ui_open()
{
    show_overlay(PAGE_MAIN);
}

void settings_ui_open_menu()
{
    show_overlay(PAGE_MENU);
}
