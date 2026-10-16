#pragma once

#include <memory>
#include <string>

/* Persistent device settings. Simulator: a JSON file (src/platform_sim/settings_file.cpp);
 * ESP32: NVS. Values are plain strings so the storage layer stays trivial. */
struct AppSettings {
    std::string server;        /* music service endpoint, "host[:port]" (tidal-hifi on the host PC) */
    std::string wifi_ssid;     /* last network the user connected to */
    std::string wifi_password;
};

class SettingsStore {
public:
    virtual ~SettingsStore()                       = default;
    virtual bool load(AppSettings & out)           = 0; /* false: nothing stored yet (out untouched) */
    virtual bool save(const AppSettings & settings) = 0;
};

/* Provided by the platform layer. */
std::unique_ptr<SettingsStore> settings_store_create();
