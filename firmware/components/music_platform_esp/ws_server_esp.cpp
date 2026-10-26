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

        httpd_config_t cfg          = HTTPD_DEFAULT_CONFIG();
        cfg.server_port             = port;
        cfg.max_open_sockets        = limits.max_connections + 3;
        cfg.stack_size              = 8192;
        cfg.global_user_ctx         = this;
        cfg.global_user_ctx_free_fn = [](void *) {};
        cfg.close_fn                = on_session_close;
        esp_err_t err               = httpd_start(&server_, &cfg);
        if(err != ESP_OK) {
            ESP_LOGE(TAG, "start failed %s", esp_err_to_name(err));
            return false;
        }
        httpd_uri_t uri{};
        uri.uri          = "/source";
        uri.method       = HTTP_GET;
        uri.handler      = on_request;
        uri.user_ctx     = this;
        uri.is_websocket = true;

        err = httpd_register_uri_handler(server_, &uri);
        if(err != ESP_OK) {
            ESP_LOGE(TAG, "register uri failed: %s", esp_err_to_name(err));
            httpd_stop(server_);
            server_ = nullptr;
            return false;
        }

        return true;
    }
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
    void close(WsConnId id) override
    {
        if(server_ == nullptr) return;
        httpd_sess_trigger_close(server_, static_cast<int>(id));
    }
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
    std::mutex send_mutex_;

    httpd_handle_t server_ = nullptr;
    WsHandlers handlers_;
    WsLimits limits_;

    static esp_err_t on_request(httpd_req_t * req)
    {
        auto * self = static_cast<EspWsServer *>(req->user_ctx);
        WsConnId id = httpd_req_to_sockfd(req);
        if(req->method == HTTP_GET) {
            std::string path = req->uri;
            if(self->handlers_.on_open && !self->handlers_.on_open(id, path)) {
                return ESP_FAIL;
            }
            return ESP_OK;
        }
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
