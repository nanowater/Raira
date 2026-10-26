#pragma once

#include <string>
#include <vector>
#include "hub_source.h"
#include "platform/settings.h"

/* What the hub needs from the saved settings at start-up, the same for the simulator and the ESP32. */
struct HubBoot
{
    AppSettings settings;             /* as saved, with the device identity filled in */
    std::vector<PairedSource> paired; /* the sources paired so far (`hub_sources`) */
};

/* Loads the settings, generates the device id and name on first use (and saves them, so they stay), and reads the
 * paired sources. `default_name` is the device name shown to sources until the user has chosen one. */
HubBoot hub_boot_load(SettingsStore & store, const std::string & default_name);
