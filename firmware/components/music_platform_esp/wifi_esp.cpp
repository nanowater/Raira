#include "esp_err.h"
#include "esp_event.h"
#include "esp_netif_types.h"
#include "esp_wifi_types_generic.h"
#include "platform/wifi.h"
#include <memory>
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_log.h"
#include <atomic>
#include <mutex>
#include <vector>
#include <algorithm>
#include <cstring>

static const char * TAG          = "wifi";
static constexpr int MAX_RETRIES = 3;

namespace {
class EspWifi : public WifiManager {
  public:
    EspWifi()
    {
        ESP_ERROR_CHECK(esp_netif_init());
        ESP_ERROR_CHECK(esp_event_loop_create_default());
        esp_netif_create_default_wifi_sta();
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        ESP_ERROR_CHECK(esp_wifi_init(&cfg));

        ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &on_event, this, nullptr));
        ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &on_event, this, nullptr));

        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_start());
    }
    void start_scan() override
    {
        scanning_                         = true;
        esp_err_t esp_wifi_scan_start_err = esp_wifi_scan_start(nullptr, false);
        if(esp_wifi_scan_start_err != ESP_OK) {
            ESP_LOGE(TAG, "scan start failed: %s", esp_err_to_name(esp_wifi_scan_start_err));
            scanning_ = false;
        }
        return;
    }
    bool scan_running() const override
    {
        return scanning_;
    }
    std::vector<WifiNetwork> scan_result() const override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return results_;
    }

    void connect(const std::string & ssid, const std::string & password) override
    {
        wifi_config_t cfg{};

        if(ssid.empty() || ssid.size() > sizeof(cfg.sta.ssid) || password.size() > sizeof(cfg.sta.password)) {
            ESP_LOGE(TAG, "invalid ssid or password length");
            state_ = WifiState::Failed;
            return;
        }

        std::memcpy(cfg.sta.ssid, ssid.data(), ssid.size());
        std::memcpy(cfg.sta.password, password.data(), password.size());

        {
            std::lock_guard<std::mutex> lock(mutex_);
            pending_ssid_ = ssid;
        }

        retries_ = 0;
        state_   = WifiState::Connecting;

        esp_wifi_disconnect();

        esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
        if(err == ESP_OK) err = esp_wifi_connect();
        if(err != ESP_OK) {
            ESP_LOGE(TAG, "connect failed: %s", esp_err_to_name(err));
            state_ = WifiState::Failed;
        }
    }
    WifiState state() const override
    {
        return state_;
    }
    std::string connected_ssid() const override
    {
        if(state_ != WifiState::Connected) {
            return "";
        }
        std::lock_guard<std::mutex> lock(mutex_);
        return pending_ssid_;
    }

  private:
    static void on_event(void * arg, esp_event_base_t base, int32_t id, void * data)
    {
        ESP_LOGI(TAG, "event %s %d", base, (int)id);
        auto * self = static_cast<EspWifi *>(arg);
        if(base == WIFI_EVENT && id == WIFI_EVENT_SCAN_DONE) self->on_scan_done();
        if(base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
            auto * info = static_cast<wifi_event_sta_disconnected_t *>(data);
            ESP_LOGW(TAG, "disconnected, reason %d", info->reason);
            self->on_disconnected();
        }

        if(base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
            self->retries_ = 0;
            self->state_   = WifiState::Connected;
        }
    }

    void on_scan_done()
    {
        uint16_t n = 0;
        esp_wifi_scan_get_ap_num(&n);
        std::vector<wifi_ap_record_t> recs(n);
        esp_wifi_scan_get_ap_records(&n, recs.data());
        recs.resize(n);

        std::vector<WifiNetwork> found;

        for(const wifi_ap_record_t & rec : recs) {
            if(rec.ssid[0] == '\0') {
                continue;
            }
            WifiNetwork net;
            net.ssid    = reinterpret_cast<const char *>(rec.ssid);
            net.rssi    = rec.rssi;
            net.secured = rec.authmode != WIFI_AUTH_OPEN;
            found.push_back(net);
        }
        std::sort(found.begin(), found.end(),
                  [](const WifiNetwork & a, const WifiNetwork & b) { return a.rssi > b.rssi; });
        {
            std::lock_guard<std::mutex> lock(mutex_);
            results_ = std::move(found);
        }
        scanning_ = false;
    }

    void on_disconnected()
    {
        switch(state_.load()) {
            case WifiState::Connecting:
                if(retries_ < MAX_RETRIES) {
                    retries_++;
                    esp_wifi_connect();
                } else {
                    state_ = WifiState::Failed;
                }
                break;
            case WifiState::Connected:
                retries_ = 0;
                state_   = WifiState::Connecting;
                esp_wifi_connect();
                break;
            case WifiState::Idle:
            case WifiState::Failed: break;
        }
    }

    std::atomic<bool> scanning_{false};
    mutable std::mutex mutex_;
    std::vector<WifiNetwork> results_;
    std::atomic<WifiState> state_{WifiState::Idle};
    std::atomic<int> retries_{0};
    std::string pending_ssid_;
};
} // namespace

std::unique_ptr<WifiManager> wifi_manager_create()
{
    return std::make_unique<EspWifi>();
}
