#include "hub_boot.h"
#include <random>
#include <nlohmann/json.hpp>

namespace {
std::string random_hex(int bytes)
{
    static const char H[] = "0123456789abcdef";
    std::random_device rd;
    std::string s;
    for(int i = 0; i < bytes; i++) {
        unsigned v = rd() & 0xFF;
        s += H[v >> 4];
        s += H[v & 15];
    }
    return s;
}
} // namespace

HubBoot hub_boot_load(SettingsStore & store, const std::string & default_name)
{
    HubBoot b;
    store.load(b.settings);
    if(b.settings.hub_device_id.empty()) b.settings.hub_device_id = random_hex(8);
    if(b.settings.hub_device_name.empty()) b.settings.hub_device_name = default_name;
    store.save(b.settings); /* keeps the generated identity */

    nlohmann::json list =
        nlohmann::json::parse(b.settings.hub_sources.empty() ? "[]" : b.settings.hub_sources, nullptr, false);
    if(list.is_array())
        for(const auto & e : list)
            if(e.is_object() && e.value("id", "") != "")
                b.paired.push_back({e.value("id", ""), e.value("name", ""), e.value("secret", "")});
    return b;
}
