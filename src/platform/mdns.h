#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

/* Announces the hub on the local network (mDNS / DNS-SD) so sources find it without being given an address.
 * Simulator: a small responder over UDP multicast (src/platform_sim/mdns_responder.cpp); ESP32: the `mdns`
 * component of ESP-IDF (mdns_hostname_set, mdns_instance_name_set, mdns_service_add). */
struct MdnsService
{
    std::string instance;                   /* "Music Controller", shown to the user */
    std::string host = "music-controller";  /* announced as <host>.local */
    std::string type = "_musicbridge._tcp"; /* service type */
    uint16_t port    = 18322;
    std::vector<std::pair<std::string, std::string>> txt; /* e.g. proto=1, id=<device id> */
};

class MdnsAdvertiser {
  public:
    virtual ~MdnsAdvertiser()                       = default;
    virtual bool start(const MdnsService & service) = 0; /* false: no usable network interface or socket */
    virtual void stop()                             = 0; /* sends the goodbye so listeners forget us at once */
};

/* Provided by the platform layer. */
std::unique_ptr<MdnsAdvertiser> mdns_advertiser_create();
