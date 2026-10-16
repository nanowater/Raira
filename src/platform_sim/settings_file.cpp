#include "platform/settings.h"
#include <cstdlib>
#include <fstream>
#include <sys/stat.h>
#include <nlohmann/json.hpp>

namespace {

std::string settings_dir()
{
    if(const char * x = getenv("XDG_CONFIG_HOME")) return std::string(x) + "/music-controller";
    if(const char * h = getenv("HOME")) return std::string(h) + "/.config/music-controller";
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
        get("server", out.server);
        get("bridge_token", out.bridge_token);
        get("player", out.player);
        get("wifi_ssid", out.wifi_ssid);
        get("wifi_password", out.wifi_password);
        return true;
    }

    bool save(const AppSettings & s) override
    {
        const std::string dir = settings_dir();
        mkdir((dir.substr(0, dir.rfind('/'))).c_str(), 0700); /* ~/.config */
        mkdir(dir.c_str(), 0700);
        nlohmann::json j = {{"server", s.server}, {"bridge_token", s.bridge_token}, {"player", s.player}, {"wifi_ssid", s.wifi_ssid}, {"wifi_password", s.wifi_password}};
        std::string path = dir + "/settings.json";
        std::ofstream f(path);
        if(!f) return false;
        f << j.dump(2) << "\n";
        f.close();
        chmod(path.c_str(), 0600); /* holds the WiFi password and the bridge token */
        return f.good();
    }
};

} // namespace

std::unique_ptr<SettingsStore> settings_store_create()
{
    return std::make_unique<FileStore>();
}
