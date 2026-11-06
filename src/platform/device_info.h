#pragma once

#include <string>
#include <vector>

/* What the Settings > About page shows about the hardware and the firmware: one line per fact, in the order they are
 * to be shown. The platform decides which facts it has (chip, memory, flash, firmware version, MAC, IP address, ...);
 * the page lists whatever comes back. Called on the LVGL thread each time the page opens, so values that change (free
 * memory, the IP address) are current; it must not block. Simulator: src/platform_sim/device_info_sim.cpp. */
struct DeviceInfoRow
{
    std::string name;  /* "Chip" */
    std::string value; /* "ESP32-P4 rev v3.2, 2 cores, 400 MHz" */
};

std::vector<DeviceInfoRow> device_info_rows();
