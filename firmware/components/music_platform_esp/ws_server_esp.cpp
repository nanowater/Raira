// WsServer on esp_http_server's WebSocket support. Needs CONFIG_HTTPD_WS_SUPPORT and
// CONFIG_HTTPD_WS_PRE_HANDSHAKE_CB_SUPPORT (firmware/sdkconfig.defaults). A connection is identified by its socket
// file descriptor.

#include "lwip/sockets.h"
#include "platform/ws_server.h"
#include "esp_err.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include <cstdint>
#include <memory>
#include <mutex>

static const char * TAG = "ws";

namespace {
// The httpd callbacks (on_handshake, on_request, on_session_close) all run on the server's own task, one at a time.
// send_*(), close() and peer() may be called from any thread.
class EspWsServer : public WsServer {
  public:
    ~EspWsServer() override
    {
        stop();
    }
    bool start(uint16_t port, WsHandlers handlers, WsLimits limits) override
    {
        if(server_ != nullptr) {
            return false;
        }
        handlers_ = handlers;
        limits_   = limits;

        httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
        cfg.server_port    = port;
        // The server keeps 3 of its sockets for itself, the rest are connections.
        cfg.max_open_sockets = limits.max_connections + 3;
        // The stack of the server task, which also runs the HubSource handlers (JSON parsing). Not measured yet, see
        // the first boot checklist in docs/PORTING.md.
        cfg.stack_size = 8192;
        // on_session_close() gets no pointer of ours, only the server handle, and finds us again through this. The
        // server free()s global_user_ctx when it stops, and `this` is not heap memory we may hand over: do nothing.
        cfg.global_user_ctx         = this;
        cfg.global_user_ctx_free_fn = [](void *) {};
        // Called for every closed session, also when the peer simply disappears. It replaces the server's own close.
        cfg.close_fn  = on_session_close;
        esp_err_t err = httpd_start(&server_, &cfg);
        if(err != ESP_OK) {
            ESP_LOGE(TAG, "start failed %s", esp_err_to_name(err));
            return false;
        }
        // Only this path accepts WebSocket connections, every other request is answered with 404.
        httpd_uri_t uri{};
        uri.uri          = "/source";
        uri.method       = HTTP_GET;
        uri.handler      = on_request;
        uri.user_ctx     = this;
        uri.is_websocket = true;
        // IDF 6 does not call the URI handler for the opening GET request, so the new connection is reported here.
        uri.ws_pre_handshake_cb = on_handshake;

        err = httpd_register_uri_handler(server_, &uri);
        if(err != ESP_OK) {
            ESP_LOGE(TAG, "register uri failed: %s", esp_err_to_name(err));
            httpd_stop(server_);
            server_ = nullptr;
            return false;
        }

        return true;
    }
    // Safe to call more than once and also when start() failed.
    void stop() override
    {
        if(server_ == nullptr) return;
        httpd_stop(server_);
        server_ = nullptr;
    }
    bool send_text(WsConnId id, const std::string & text) override
    {
        return send_frame(id, HTTPD_WS_TYPE_TEXT, text);
    }
    bool send_binary(WsConnId id, const std::string & data) override
    {
        return send_frame(id, HTTPD_WS_TYPE_BINARY, data);
    }
    // Asks the server to close the session; the close then comes back through on_session_close() as on_close.
    void close(WsConnId id) override
    {
        if(server_ == nullptr) return;
        httpd_sess_trigger_close(server_, static_cast<int>(id));
    }
    // The peer's IPv4 address as text, empty when it cannot be found. IPv6 is not handled.
    std::string peer(WsConnId id) override
    {
        sockaddr_in addr{};
        socklen_t len = sizeof(addr);
        if(getpeername(static_cast<int>(id), reinterpret_cast<sockaddr *>(&addr), &len) != 0) {
            return "";
        }
        char buf[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &addr.sin_addr, buf, sizeof(buf));
        return buf;
    }

  private:
    // Several threads send (the poller, the UI, the hub); one frame at a time.
    std::mutex send_mutex_;

    httpd_handle_t server_ = nullptr;
    WsHandlers handlers_;
    WsLimits limits_;

    // Called once for every frame that arrives (not for the handshake, see on_handshake). The frame is read in two
    // steps: httpd_ws_recv_frame() with length 0 only fills in the header, so that the size is known before a buffer
    // is allocated; the second call reads the payload. A frame above limits_.max_message ends the connection (ESP_FAIL
    // makes the server close it). Only text and binary frames are passed on.
    static esp_err_t on_request(httpd_req_t * req)
    {
        auto * self = static_cast<EspWsServer *>(req->user_ctx);
        WsConnId id = httpd_req_to_sockfd(req);
        httpd_ws_frame_t frame{};
        esp_err_t err = httpd_ws_recv_frame(req, &frame, 0);
        if(err != ESP_OK) {
            ESP_LOGE(TAG, "recv frame header failed: %s", esp_err_to_name(err));
            return err;
        }
        if(frame.len > self->limits_.max_message) {
            return ESP_FAIL;
        }
        std::string data(frame.len, '\0');
        if(frame.len > 0) {
            frame.payload = reinterpret_cast<uint8_t *>(data.data());
            err           = httpd_ws_recv_frame(req, &frame, frame.len);
            if(err != ESP_OK) return ESP_FAIL;
        }
        if(frame.type == HTTPD_WS_TYPE_TEXT && self->handlers_.on_text) {
            self->handlers_.on_text(id, data);
        } else if(frame.type == HTTPD_WS_TYPE_BINARY && self->handlers_.on_binary) {
            self->handlers_.on_binary(id, data);
        }
        return ESP_OK;
    }

    // Runs while the WebSocket handshake is processed. Anything but ESP_OK closes the connection, which is how on_open
    // turns a source away (for example a wrong path).
    static esp_err_t on_handshake(httpd_req_t * req)
    {
        auto * self      = static_cast<EspWsServer *>(req->user_ctx);
        WsConnId id      = httpd_req_to_sockfd(req);
        std::string path = req->uri;
        if(self->handlers_.on_open && !self->handlers_.on_open(id, path)) return ESP_FAIL;
        return ESP_OK;
    }

    // httpd_ws_send_data() needs only the server handle and the socket, so it works from any thread; the _async
    // variants want a request object, which a sender outside the handlers does not have. The API takes a non-const
    // payload pointer but does not change the data.
    bool send_frame(WsConnId id, httpd_ws_type_t type, const std::string & data)
    {
        std::lock_guard<std::mutex> lock(send_mutex_);
        if(server_ == nullptr) return false;
        httpd_ws_frame_t frame{};
        frame.type    = type;
        frame.payload = reinterpret_cast<uint8_t *>(const_cast<char *>(data.data()));
        frame.len     = data.size();
        return httpd_ws_send_data(server_, static_cast<int>(id), &frame) == ESP_OK;
    }
    // Tells the hub the connection is gone, then closes the socket: with a close_fn set, that is our job.
    static void on_session_close(httpd_handle_t hd, int fd)
    {
        auto * self = static_cast<EspWsServer *>(httpd_get_global_user_ctx(hd));
        if(self != nullptr && self->handlers_.on_close) self->handlers_.on_close(static_cast<WsConnId>(fd));
        ::close(fd);
    }
};
} // namespace

std::unique_ptr<WsServer> ws_server_create()
{
    return std::make_unique<EspWsServer>();
}
