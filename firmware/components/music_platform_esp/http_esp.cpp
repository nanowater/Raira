// platform/http.h on the ESP32: esp_http_client. Behaves like the simulator's libcurl client (src/platform_sim/http_curl.cpp): blocking,
// one connection kept open between the calls of one client (keep-alive), redirects followed, HTTPS checked against ESP-IDF's
// certificate bundle, and a request that is cancelled (HttpRequest::cancelled) comes back with transport_ok == false.
#include <string>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "platform/http.h"

namespace {

const char * TAG = "http";

// what the event handler of one perform() works with
struct Ctx {
    std::string * body;
    const std::function<bool()> * cancelled;
    bool aborted = false;
};

esp_err_t on_event(esp_http_client_event_t * e)
{
    auto * c = static_cast<Ctx *>(e->user_data);
    if(!c) return ESP_OK;
    if(e->event_id == HTTP_EVENT_ON_DATA && e->data && e->data_len > 0) c->body->append(static_cast<const char *>(e->data), (size_t)e->data_len);
    // The only places where a blocking request gives control back are its events. A transfer that stalls without any event ends with
    // the timeout instead.
    if(!c->aborted && c->cancelled && *c->cancelled && (*c->cancelled)()) {
        c->aborted = true;
        esp_http_client_cancel_request(e->client); // closes the socket: the running perform() fails
    }
    return ESP_OK;
}

class EspClient : public HttpClient {
public:
    ~EspClient() override
    {
        if(h_) esp_http_client_cleanup(h_);
    }

    HttpResponse perform(const HttpRequest & req) override
    {
        HttpResponse res;
        Ctx ctx{&res.body, &req.cancelled};

        if(!h_) {
            esp_http_client_config_t cfg = {};
            cfg.url                      = req.url.c_str();
            cfg.event_handler            = on_event;
            cfg.crt_bundle_attach        = esp_crt_bundle_attach;
            cfg.keep_alive_enable        = true;
            cfg.timeout_ms               = req.timeout_s * 1000;
            h_                           = esp_http_client_init(&cfg);
            if(!h_) {
                ESP_LOGE(TAG, "esp_http_client_init failed");
                return res;
            }
        }
        esp_http_client_set_user_data(h_, &ctx);
        esp_http_client_set_url(h_, req.url.c_str());
        esp_http_client_set_timeout_ms(h_, req.timeout_s * 1000);

        // the same handle serves every request of this client: set all, and delete what an earlier request left behind
        esp_http_client_set_method(h_, req.method == "POST" ? HTTP_METHOD_POST : req.method == "PUT" ? HTTP_METHOD_PUT : HTTP_METHOD_GET);
        set_or_delete("User-Agent", req.user_agent);
        set_or_delete("Content-Type", req.body.empty() ? std::string() : req.content_type);
        set_or_delete("Authorization", req.bearer_token.empty() ? std::string() : "Bearer " + req.bearer_token);
        if(req.method == "GET") esp_http_client_set_post_field(h_, nullptr, 0);
        else esp_http_client_set_post_field(h_, req.body.c_str(), (int)req.body.size()); // req outlives perform()

        const esp_err_t err = esp_http_client_perform(h_);
        res.status          = esp_http_client_get_status_code(h_);
        res.transport_ok    = (err == ESP_OK) && !ctx.aborted;
        if(!res.transport_ok) {
            if(!ctx.aborted) ESP_LOGW(TAG, "%s %s failed: %s", req.method.c_str(), req.url.c_str(), esp_err_to_name(err));
            res.status = 0;
        }
        esp_http_client_set_user_data(h_, nullptr); // ctx goes out of scope
        return res;
    }

private:
    void set_or_delete(const char * key, const std::string & value)
    {
        if(value.empty()) esp_http_client_delete_header(h_, key);
        else esp_http_client_set_header(h_, key, value.c_str());
    }

    esp_http_client_handle_t h_ = nullptr;
};

} // namespace

std::unique_ptr<HttpClient> http_client_create()
{
    return std::make_unique<EspClient>();
}
