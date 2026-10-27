/* SettingsStore as a JSON file under $XDG_CONFIG_HOME/raira or ~/.config/raira (the simulator's counterpart of NVS). */
#include "platform/settings.h"
#include <cstdlib>
#include <fstream>
#include <sys/stat.h>
#include <nlohmann/json.hpp>

namespace {

std::string settings_dir()
{
    if(const char * x = getenv("XDG_CONFIG_HOME")) return std::string(x) + "/raira";
    if(const char * h = getenv("HOME")) return std::string(h) + "/.config/raira";
    return ".";
}

class FileStore : public SettingsStore {
  public:
    bool load(AppSettings & out) override
    {
        std::ifstream f(settings_dir() + "/settings.json");
        if(!f) return false;
        nlohmann::json j = nlohmann::json::parse(f, nullptr, false);
        if(j.is_discarded() || !j.is_object()) return false;
        auto get = [&](const char * k, std::string & dst) {
            auto it = j.find(k);
            if(it != j.end() && it->is_string()) dst = it->get<std::string>();
        };
        get("player", out.player);
        get("hub_device_id", out.hub_device_id);
        get("hub_device_name", out.hub_device_name);
        get("hub_sources", out.hub_sources);
        get("wifi_ssid", out.wifi_ssid);
        get("wifi_password", out.wifi_password);
        return true;
    }

    bool save(const AppSettings & s) override
    {
        const std::string dir = settings_dir();
        mkdir((dir.substr(0, dir.rfind('/'))).c_str(), 0700); /* ~/.config */
        mkdir(dir.c_str(), 0700);
        nlohmann::json j = {
            {"player", s.player},           {"hub_device_id", s.hub_device_id}, {"hub_device_name", s.hub_device_name},
            {"hub_sources", s.hub_sources}, {"wifi_ssid", s.wifi_ssid},         {"wifi_password", s.wifi_password}};
        std::string path = dir + "/settings.json";
        std::ofstream f(path);
        if(!f) return false;
        f << j.dump(2) << "\n";
        f.close();
        chmod(path.c_str(), 0600); /* holds the WiFi password and the pairing secrets */
        return f.good();
    }
};

} // namespace

std::unique_ptr<SettingsStore> settings_store_create()
{
    return std::make_unique<FileStore>();
}
