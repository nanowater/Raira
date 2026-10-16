#include "settings_ui.h"
#include <cstring>
#include <vector>
#include "media_poller.h"
#include "strings.h"
#include "ui_fonts.h"

namespace {

enum Page { PAGE_MAIN, PAGE_WIFI, PAGE_PASSWORD, PAGE_SERVER, PAGE_TOKEN, PAGE_COUNT };

const uint32_t COLOR_BG      = 0x12151B;
const uint32_t COLOR_ROW     = 0x1F2937;
const uint32_t COLOR_ROW_PRESSED = 0x374151;
const uint32_t COLOR_DIM     = 0x9CA3AF;
const uint32_t COLOR_ACCENT  = 0x60A5FA;
const uint32_t COLOR_GOOD    = 0x34D399;
const uint32_t COLOR_BAD     = 0xF87171;

SettingsServices svc;
lv_obj_t * root        = nullptr;
lv_obj_t * title_lbl   = nullptr;
lv_obj_t * pages[PAGE_COUNT] = {};
int page_now           = PAGE_MAIN;
lv_timer_t * poll_timer = nullptr;

/* main page */
lv_obj_t * wifi_value  = nullptr;
lv_obj_t * server_value = nullptr;
lv_obj_t * token_value  = nullptr;
lv_obj_t * token_row    = nullptr;
lv_obj_t * wifi_row    = nullptr;

/* wifi page */
lv_obj_t * wifi_status = nullptr;
lv_obj_t * wifi_list   = nullptr;
bool scan_was_running  = false;
std::string selected_ssid;       /* network being configured */
std::string pending_password;    /* password of the connection attempt in progress */
bool wait_for_connect  = false;

/* password / server pages share one keyboard */
lv_obj_t * keyboard    = nullptr;
lv_obj_t * pw_ssid_lbl = nullptr;
lv_obj_t * pw_input    = nullptr;
lv_obj_t * pw_eye_lbl  = nullptr;
lv_obj_t * server_input = nullptr;
lv_obj_t * server_msg  = nullptr;
lv_obj_t * token_input = nullptr;
lv_obj_t * token_msg   = nullptr;

const lv_font_t * f(int px, bool bold = false)
{
    return ui_noto_font(px, px >= 28 ? &lv_font_montserrat_28 : &lv_font_montserrat_22, bold);
}

void show_page(int p);

/* One row of the network list; freed when the row is deleted. */
struct NetRow {
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
lv_obj_t * make_button(lv_obj_t * parent, const char * text, int w, int h, uint32_t bg, lv_event_cb_t cb, void * user = nullptr)
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

lv_obj_t * make_row(lv_obj_t * parent, int y, const char * title, lv_obj_t ** value_out, lv_event_cb_t cb)
{
    lv_obj_t * r = lv_obj_create(parent);
    lv_obj_set_size(r, 740, 76);
    lv_obj_set_pos(r, 30, y);
    lv_obj_set_style_radius(r, 14, 0);
    lv_obj_set_style_bg_color(r, lv_color_hex(COLOR_ROW), 0);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(r, lv_color_hex(COLOR_ROW_PRESSED), LV_STATE_PRESSED);
    lv_obj_set_style_border_width(r, 0, 0);
    lv_obj_set_style_pad_hor(r, 24, 0);
    lv_obj_set_scrollable(r, false);
    lv_obj_add_event_cb(r, cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t * t = make_label(r, title, 24, 0xFFFFFF);
    lv_obj_align(t, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_t * chev = make_label(r, LV_SYMBOL_RIGHT, 22, COLOR_DIM);
    lv_obj_align(chev, LV_ALIGN_RIGHT_MID, 0, 0);
    *value_out = make_label(r, "", 20, COLOR_DIM);
    lv_obj_align(*value_out, LV_ALIGN_RIGHT_MID, -36, 0);
    return r;
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
        default: return str::SETTINGS;
    }
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
        lv_obj_add_event_cb(r, [](lv_event_t * e) { delete static_cast<NetRow *>(lv_event_get_user_data(e)); }, LV_EVENT_DELETE, nr);

        lv_obj_t * name = make_label(r, n.ssid.c_str(), 22, 0xFFFFFF);
        lv_obj_align(name, LV_ALIGN_LEFT_MID, 44, 0);
        lv_obj_t * sig = make_label(r, LV_SYMBOL_WIFI, 22, n.rssi > -60 ? COLOR_GOOD : (n.rssi > -75 ? COLOR_ACCENT : COLOR_DIM));
        lv_obj_align(sig, LV_ALIGN_LEFT_MID, 0, 0);
        lv_obj_t * tag = make_label(r, n.secured ? str::TAG_SECURED : (n.ssid == current ? str::TAG_CONNECTED : str::TAG_OPEN), 16, COLOR_DIM);
        lv_obj_align(tag, LV_ALIGN_RIGHT_MID, 0, 0);
        if(n.ssid == current) {
            lv_obj_set_style_border_width(r, 2, 0);
            lv_obj_set_style_border_color(r, lv_color_hex(COLOR_GOOD), 0);
        }
    }
}

void start_scan()
{
    if(!svc.wifi) return;
    svc.wifi->start_scan();
    scan_was_running = true;
    lv_obj_clean(wifi_list);
    set_status(wifi_status, str::SCANNING, COLOR_DIM);
}

void poll_cb(lv_timer_t *)
{
    if(!svc.wifi) return;
    if(page_now == PAGE_WIFI && scan_was_running && !svc.wifi->scan_running()) {
        scan_was_running = false;
        fill_wifi_list();
        set_status(wifi_status, svc.wifi->state() == WifiState::Connected ? (std::string(str::CONNECTED_TO) + svc.wifi->connected_ssid()) : str::SELECT_NETWORK,
                   svc.wifi->state() == WifiState::Connected ? COLOR_GOOD : COLOR_DIM);
    }
    if(wait_for_connect) {
        switch(svc.wifi->state()) {
            case WifiState::Connecting: set_status(wifi_status, std::string(str::CONNECTING_TO) + selected_ssid + " ...", COLOR_ACCENT); break;
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

void on_back(lv_event_t *)
{
    switch(page_now) {
        case PAGE_PASSWORD: show_page(PAGE_WIFI); break;
        case PAGE_WIFI:
        case PAGE_SERVER:
        case PAGE_TOKEN: show_page(PAGE_MAIN); break;
        default:
            lv_obj_set_hidden(root, true);
            if(poll_timer) lv_timer_pause(poll_timer);
            break;
    }
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
    if(svc.set_token) svc.set_token(lv_textarea_get_text(token_input)); /* the result appears via on_connection_state() */
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
        case MEDIA_CONN_UNKNOWN:   set_status(msg, str::SERVER_CHECKING, COLOR_DIM); break;
        case MEDIA_CONN_CONNECTED: set_status(msg, str::SERVER_CONNECTED, COLOR_GOOD); break;
        case MEDIA_CONN_LOST:      set_status(msg, str::SERVER_UNREACHABLE, COLOR_BAD); break;
    }
}

/* Enter key on the keyboard */
void on_keyboard_ready(lv_event_t *)
{
    if(page_now == PAGE_PASSWORD) connect_with_password();
    else if(page_now == PAGE_SERVER) save_server();
    else if(page_now == PAGE_TOKEN) save_token();
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
    svc = std::move(services);
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

    /* main page */
    if(svc.wifi) wifi_row = make_row(pages[PAGE_MAIN], 16, "WiFi", &wifi_value, on_open_wifi);
    else wifi_value = lv_label_create(pages[PAGE_MAIN]);
    lv_obj_t * server_row = make_row(pages[PAGE_MAIN], 108, str::SERVER_ROW, &server_value, on_open_server);
    (void)server_row;
    if(svc.get_token) token_row = make_row(pages[PAGE_MAIN], 200, str::TOKEN_ROW, &token_value, on_open_token);

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

void settings_ui_open()
{
    if(!root) return;
    show_page(PAGE_MAIN);
    lv_obj_set_hidden(root, false);
    lv_obj_move_foreground(root);
    lv_timer_resume(poll_timer);
}
