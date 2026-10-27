// SettingsStore on NVS (the flash's key-value store): one string per setting, see AppSettings in platform/settings.h.
// nvs_flash_init() has to have run first (app_main).

#include "esp_err.h"
#include "platform/settings.h"
#include <memory>
#include "nvs.h"
#include "esp_log.h"

static const char * TAG = "settings";
// NVS namespace of all settings. It is stored on the chip: renaming it would hide what an installed device has saved.
static const char * NS = "music";

namespace {
class NvsStore : public SettingsStore {
  public:
    // false when nothing was saved yet (the namespace does not exist on a fresh chip). A key that is missing leaves its
    // field at the value `out` already has, so new fields keep their defaults on a device with older settings.
    bool load(AppSettings & out) override
    {
        nvs_handle_t h;
        esp_err_t err = nvs_open(NS, NVS_READONLY, &h);

        if(err == ESP_ERR_NVS_NOT_FOUND) {
            return false;
        }
        if(err != ESP_OK) {
            ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
            return false;
        }

        get(h, "player", out.player);
        get(h, "hub_device_id", out.hub_device_id);
        get(h, "hub_device_name", out.hub_device_name);
        get(h, "hub_sources", out.hub_sources);
        get(h, "wifi_ssid", out.wifi_ssid);
        get(h, "wifi_password", out.wifi_password);

        nvs_close(h);

        return true;
    }
    // Writes every field, then commits once; the first error stops the chain. The WiFi password is stored as it is: NVS
    // is not encrypted here. Keys may have at most 15 characters, and one string value at most about 4000 bytes, which
    // is also the room the JSON list of paired sources (hub_sources) has.
    bool save(const AppSettings & settings) override
    {
        nvs_handle_t h;
        esp_err_t err = open_rw(&h);
        if(err != ESP_OK) {
            ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
            return false;
        }

        if(err == ESP_OK) err = set(h, "player", settings.player);
        if(err == ESP_OK) err = set(h, "hub_device_id", settings.hub_device_id);
        if(err == ESP_OK) err = set(h, "hub_device_name", settings.hub_device_name);
        if(err == ESP_OK) err = set(h, "hub_sources", settings.hub_sources);
        if(err == ESP_OK) err = set(h, "wifi_ssid", settings.wifi_ssid);
        if(err == ESP_OK) err = set(h, "wifi_password", settings.wifi_password);

        if(err == ESP_OK) err = nvs_commit(h);

        nvs_close(h);

        if(err != ESP_OK) {
            ESP_LOGE(TAG, "save failed: %s", esp_err_to_name(err));
            return false;
        }
        return true;
    }

  private:
    esp_err_t open_rw(nvs_handle_t * handle)
    {
        return nvs_open(NS, NVS_READWRITE, handle);
    }

    // Reads a string in two steps: the first call, without a buffer, returns the length including the terminating NUL.
    // `value` is only touched when the read succeeded.
    esp_err_t get(nvs_handle_t h, const char * key, std::string & value)
    {
        size_t len    = 0;
        esp_err_t err = nvs_get_str(h, key, nullptr, &len);
        if(err != ESP_OK) return err;
        std::string buf(len, '\0');
        err = nvs_get_str(h, key, buf.data(), &len);
        if(err != ESP_OK) return err;
        buf.resize(len - 1);
        value = std::move(buf);
        return ESP_OK;
    }

    esp_err_t set(nvs_handle_t h, const char * key, const std::string & value)
    {
        return nvs_set_str(h, key, value.c_str());
    }
};
} // namespace

std::unique_ptr<SettingsStore> settings_store_create()
{
    return std::make_unique<NvsStore>();
}
