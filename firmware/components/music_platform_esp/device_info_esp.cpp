/* platform/device_info.h for the ESP32: what Settings > About lists. One DeviceInfoRow per fact, in the order they are
 * shown. Called on the LVGL thread each time the page opens, so it must be quick and must not block. */
#include "platform/device_info.h"
#include <cstdint>
#include <cstdio>
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_psram.h"
#include "sdkconfig.h"

namespace {

/* Chip: "ESP32-P4 v3.2, 2 cores". The revision is major * 100 + minor (302 = v3.2). */
std::string chip_text()
{
    esp_chip_info_t info;
    esp_chip_info(&info);
    const char * model = info.model == CHIP_ESP32P4 ? "ESP32-P4" : "ESP32";
    char text[64];
    std::snprintf(text, sizeof text, "%s v%d.%d, %d cores", model, info.revision / 100, info.revision % 100,
                  info.cores);
    return text;
}

/* The configured clock (the firmware never changes it), not a measurement. */
std::string cpu_text()
{
    return std::to_string(CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ) + " MHz";
}

std::string flash_text()
{
    uint32_t size = 0;
    if(esp_flash_get_size(nullptr, &size) != ESP_OK || size == 0) return "unknown";
    return std::to_string(size / (1024 * 1024)) + " MB";
}

/* esp_psram_get_size() is 0 when the PSRAM is not there. */
std::string psram_text()
{
    const size_t size = esp_psram_get_size();
    return size == 0 ? "none" : std::to_string(size / (1024 * 1024)) + " MB";
}

/* The internal RAM is the scarce resource of this firmware (ESP-Hosted takes its buffers from it), so show how much of
 * it is free. */
std::string memory_text()
{
    const size_t free_kb  = heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024;
    const size_t total_kb = heap_caps_get_total_size(MALLOC_CAP_INTERNAL) / 1024;
    return "internal " + std::to_string(free_kb) + " / " + std::to_string(total_kb) + " KB free";
}

std::string firmware_text()
{
    return esp_app_get_description()->version;
}

std::string mac_text()
{
    uint8_t mac[6] = {};
    if(esp_efuse_mac_get_default(mac) != ESP_OK) return "unknown";
    char text[24];
    std::snprintf(text, sizeof text, "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return text;
}

/* "" while there is no WiFi (no netif yet, or the address is still 0.0.0.0): the row is then left out. */
std::string ip_text()
{
    esp_netif_t * netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if(!netif) return "";
    esp_netif_ip_info_t ip;
    if(esp_netif_get_ip_info(netif, &ip) != ESP_OK || ip.ip.addr == 0) return "";
    char text[16];
    std::snprintf(text, sizeof text, IPSTR, IP2STR(&ip.ip));
    return text;
}

} // namespace

std::vector<DeviceInfoRow> device_info_rows()
{
    std::vector<DeviceInfoRow> rows;
    rows.push_back({"Chip", chip_text()});
    rows.push_back({"CPU", cpu_text()});
    rows.push_back({"Flash", flash_text()});
    rows.push_back({"PSRAM", psram_text()});
    rows.push_back({"Memory", memory_text()});
    rows.push_back({"Firmware", firmware_text()});
    rows.push_back({"MAC", mac_text()});
    const std::string ip = ip_text();
    if(!ip.empty()) rows.push_back({"IP address", ip});
    return rows;
}
