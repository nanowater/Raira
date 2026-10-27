// MdnsAdvertiser on the ESP-IDF mdns component (a managed dependency, see idf_component.yml): announces the hub as
// _raira._tcp so that the PC bridge finds it.

#include "esp_log.h"
#include "mdns.h"
#include "platform/mdns.h"
#include <memory>
#include <vector>

static const char * TAG = "mdns";

namespace {
class EspMdns : public MdnsAdvertiser {
  public:
    // Needs a network: call it after WiFi has an IP address. Our service type is one string ("_raira._tcp"), the
    // component wants the type and the protocol separately. The TXT items only point into `service`, which the call
    // keeps alive; mdns_service_add() copies them.
    bool start(const MdnsService & service) override
    {
        const size_t dot = service.type.find('.');
        if(dot == std::string::npos) {
            ESP_LOGE(TAG, "bad service type");
            return false;
        }
        const std::string type  = service.type.substr(0, dot);
        const std::string proto = service.type.substr(dot + 1);

        std::vector<mdns_txt_item_t> txt;
        for(const auto & [key, value] : service.txt) txt.push_back({key.c_str(), value.c_str()});

        esp_err_t err = mdns_init();
        if(err == ESP_OK) err = mdns_hostname_set(service.host.c_str());
        if(err == ESP_OK) err = mdns_instance_name_set(service.instance.c_str());
        if(err == ESP_OK)
            err = mdns_service_add(service.instance.c_str(), type.c_str(), proto.c_str(), service.port, txt.data(),
                                   txt.size());
        // The first error stops the chain; a failed start leaves the component initialised (stop() frees it).
        if(err != ESP_OK) {
            ESP_LOGE(TAG, "mdns start failed: %s", esp_err_to_name(err));
            return false;
        }
        return true;
    }
    void stop() override
    {
        mdns_free();
    }
};
} // namespace

std::unique_ptr<MdnsAdvertiser> mdns_advertiser_create()
{
    return std::make_unique<EspMdns>();
}
