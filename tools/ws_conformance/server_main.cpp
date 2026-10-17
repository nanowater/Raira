// Test harness around the simulator's WsServer (src/platform_sim/ws_server_lws.cpp): echoes text and binary frames and only
// accepts the path /source. Usage: ws_server <port> [seconds to run]
#include "platform/ws_server.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

int main(int argc, char ** argv)
{
    if(argc < 2) return 2;
    auto s        = ws_server_create();
    WsServer * sp = s.get();
    WsHandlers h;
    h.on_open   = [](WsConnId, const std::string & path) { printf("open %s\n", path.c_str()); fflush(stdout); return path == "/source"; };
    h.on_text   = [sp](WsConnId id, const std::string & t) { printf("text %zu\n", t.size()); fflush(stdout); sp->send_text(id, "echo:" + t); };
    h.on_binary = [sp](WsConnId id, const std::string & d) { printf("binary %zu\n", d.size()); fflush(stdout); sp->send_binary(id, d); };
    h.on_close  = [](WsConnId) { printf("close\n"); fflush(stdout); };
    if(!s->start((uint16_t)atoi(argv[1]), h, WsLimits{})) {
        printf("cannot listen\n");
        return 1;
    }
    printf("listening\n");
    fflush(stdout);
    std::this_thread::sleep_for(std::chrono::seconds(argc > 2 ? atoi(argv[2]) : 20));
    s->stop();
}
