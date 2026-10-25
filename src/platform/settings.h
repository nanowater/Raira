#pragma once

#include <memory>
#include <string>

/* Persistent device settings. Simulator: a JSON file (src/platform_sim/settings_file.cpp);
 * ESP32: NVS. Values are plain strings so the storage layer stays trivial. */
struct AppSettings
{
    std::string player;        /* the player pinned in Settings > Player ("<source id>/<player id>"), "" = automatic */
    std::string hub_device_id; /* the hub's identity, generated on first use */
    std::string hub_device_name; /* what sources show in their device list */
    std::string hub_sources;     /* paired sources as JSON [{id,name,secret}] (secrets: keep the file private) */
    std::string wifi_ssid;       /* last network the user connected to */
    std::string wifi_password;
};

class SettingsStore {
  public:
    virtual ~SettingsStore()                        = default;
    virtual bool load(AppSettings & out)            = 0; /* false: nothing stored yet (out untouched) */
    virtual bool save(const AppSettings & settings) = 0;
};

/* Provided by the platform layer. */
std::unique_ptr<SettingsStore> settings_store_create();
