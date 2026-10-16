#include "music_app.h"
#include "browse_worker.h"
#include "media_poller.h"
#include "music_ui.h"
#include "settings_ui.h"

namespace {
/* The app lives for the whole run; these are touched from the LVGL thread only. */
std::unique_ptr<WifiManager> g_wifi;
std::unique_ptr<SettingsStore> g_store;
AppSettings g_settings;
std::function<bool(const std::string &)> g_validate;
} // namespace

void music_app_start(MusicAppDeps deps)
{
    g_wifi     = std::move(deps.wifi);
    g_store    = std::move(deps.store);
    g_settings = std::move(deps.settings);
    g_validate = std::move(deps.validate_server);

    music_ui_create();
    const media_browse_mode_t browse_mode =
        !deps.browse ? MEDIA_BROWSE_NONE : deps.browse->always_available() ? MEDIA_BROWSE_ALWAYS : MEDIA_BROWSE_WHEN_REPORTED;
    media_poller_start(std::move(deps.source), browse_mode);
    browse_worker_start(std::move(deps.browse));

    SettingsServices svc;
    svc.wifi       = g_wifi.get();
    svc.get_server = [] { return g_settings.server; };
    svc.set_server = [](const std::string & spec) {
        if(spec.empty() || (g_validate && !g_validate(spec))) return false;
        g_settings.server = spec;
        if(g_store) g_store->save(g_settings);
        media_poller_set_endpoint(spec);
        browse_worker_set_endpoint(spec);
        return true;
    };
    svc.remember_wifi = [](const std::string & ssid, const std::string & password) {
        g_settings.wifi_ssid     = ssid;
        g_settings.wifi_password = password;
        if(g_store) g_store->save(g_settings);
    };
    settings_ui_init(std::move(svc)); /* after the main UI so the overlay sits on top */
}
