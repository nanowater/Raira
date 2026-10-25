#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

/* A small WebSocket server (RFC 6455) for the hub: sources connect to it. The simulator implements it over POSIX
 * sockets (src/platform_sim/ws_server_posix.cpp); the ESP32 build uses esp_http_server's WebSocket support (same
 * interface). Plain ws:// only.
 *
 * The handlers run on the server's own thread(s), never the LVGL thread: they must not block for long, and must hand
 * UI work to media_post_to_ui(). send_*()/close() may be called from any thread. */
using WsConnId = uint32_t;

struct WsHandlers
{
    std::function<bool(WsConnId, const std::string & path)> on_open; /* return false to refuse the connection */
    std::function<void(WsConnId, const std::string & text)> on_text;
    std::function<void(WsConnId, const std::string & data)> on_binary;
    std::function<void(WsConnId)> on_close;
};

struct WsLimits
{
    size_t max_connections = 4;
    size_t max_message     = 256 * 1024; /* larger frames close the connection */
};

class WsServer {
  public:
    virtual ~WsServer()                                                     = default;
    virtual bool start(uint16_t port, WsHandlers handlers, WsLimits limits) = 0; /* false: cannot listen */
    virtual void stop()                                                     = 0;
    virtual bool send_text(WsConnId id, const std::string & text)           = 0; /* false: gone or send failed */
    virtual bool send_binary(WsConnId id, const std::string & data)         = 0;
    virtual void close(WsConnId id)                                         = 0;
    virtual std::string peer(WsConnId id) = 0; /* "ip" of the remote end, for logs/UI */
};

/* Provided by the platform layer. */
std::unique_ptr<WsServer> ws_server_create();
