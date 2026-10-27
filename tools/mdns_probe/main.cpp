// Test harness around the simulator's mDNS responder (src/platform_sim/mdns_responder.cpp): announces a test service,
// then stops cleanly (which sends the goodbye). Usage: mdns_test <port> [seconds to run]
#include "platform/mdns.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

int main(int argc, char ** argv)
{
    if(argc < 2) return 2;
    auto m = mdns_advertiser_create();
    MdnsService s;
    s.instance = "Raira (probe)";
    s.host     = "raira-probe";
    s.port     = (uint16_t)atoi(argv[1]);
    s.txt      = {{"proto", "1"}, {"id", "probe123"}, {"name", "Raira (probe)"}};
    if(!m->start(s)) {
        printf("cannot start\n");
        return 1;
    }
    printf("announcing\n");
    fflush(stdout);
    std::this_thread::sleep_for(std::chrono::seconds(argc > 2 ? atoi(argv[2]) : 20));
    m->stop();
    printf("stopped\n");
}
