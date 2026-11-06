/* platform/device_info.h for the simulator: it is not a device, so the page shows the host it runs on. */
#include "platform/device_info.h"
#include <cstdio>
#include <thread>
#include <unistd.h>

std::vector<DeviceInfoRow> device_info_rows()
{
    std::vector<DeviceInfoRow> rows;
    rows.push_back({"Chip", "Simulator (" + std::to_string(std::thread::hardware_concurrency()) + " threads)"});
    const long pages = sysconf(_SC_PHYS_PAGES), page_size = sysconf(_SC_PAGESIZE);
    if(pages > 0 && page_size > 0) {
        char text[48];
        std::snprintf(text, sizeof text, "%.1f GB (host)", double(pages) * double(page_size) / (1024.0 * 1024 * 1024));
        rows.push_back({"Memory", text});
    }
    rows.push_back({"Firmware", "simulator"});
    return rows;
}
