/* music_app_start(): builds the UI, starts the poller and the browse worker and connects the settings page to the
 * settings store, the WiFi manager and the hub. Nothing in here depends on the platform. */
#include "music_app.h"
#include "browse_worker.h"
#include "media_poller.h"
#include "music_ui.h"
#include "settings_ui.h"
#include <nlohmann/json.hpp>

namespace {
/* The app lives for the whole run; these are touched from the LVGL thread only. */
std::unique_ptr<WifiManager> g_wifi;
std::unique_ptr<SettingsStore> g_store;
AppSettings g_settings;
} // namespace

/* Wires the portable app together; the host program (main() or app_main) calls this once, on the LVGL thread. Order
 * matters: the main UI exists before the poller starts feeding it, and the settings overlay comes last so that it
 * sits on top. Settings are written from the LVGL thread only; the hub's pairing callback arrives on the server's
 * thread and is posted over. `deps.source` is read (can_select_player) before it is handed to the poller. */
void music_app_start(MusicAppDeps deps)
{
    g_wifi     = std::move(deps.wifi);
    g_store    = std::move(deps.store);
    g_settings = std::move(deps.settings);

    const bool picks_player = deps.source->can_select_player();
    HubSource * deps_hub    = deps.hub;
    if(deps_hub) {
        /* the hub reports changes from its server thread: persist them on the LVGL thread */
        deps_hub->set_paired_changed_cb([](const std::vector<PairedSource> & list) {
            nlohmann::json j = nlohmann::json::array();
            for(const PairedSource & p : list) j.push_back({{"id", p.id}, {"name", p.name}, {"secret", p.secret}});
            media_post_to_ui([text = j.dump()] {
                g_settings.hub_sources = text;
                if(g_store) g_store->save(g_settings);
            });
        });
    }
    music_ui_create();
    const media_browse_mode_t browse_mode = !deps.browse                      ? MEDIA_BROWSE_NONE
                                            : deps.browse->always_available() ? MEDIA_BROWSE_ALWAYS
                                                                              : MEDIA_BROWSE_WHEN_REPORTED;
    media_poller_start(std::move(deps.source), browse_mode);
    browse_worker_start(std::move(deps.browse));

    SettingsServices svc;
    svc.wifi          = g_wifi.get();
    svc.hub           = deps_hub;
    svc.remember_wifi = [](const std::string & ssid, const std::string & password) {
        g_settings.wifi_ssid     = ssid;
        g_settings.wifi_password = password;
        if(g_store) g_store->save(g_settings);
    };
    if(picks_player) {
        svc.get_player = [] { return g_settings.player; };
        svc.set_player = [](const std::string & id) {
            g_settings.player = id;
            if(g_store) g_store->save(g_settings);
            media_poller_set_player(id);
        };
        svc.list_players = [](media_players_cb_t done) { media_poller_list_players(std::move(done)); };
    }
    settings_ui_init(std::move(svc)); /* after the main UI so the overlay sits on top */
}
