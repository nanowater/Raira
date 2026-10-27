/* The hub's start-up from the saved settings, shared by the simulator and the ESP32 (hub_boot.h). */
#include "hub_boot.h"
#include <nlohmann/json.hpp>

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
