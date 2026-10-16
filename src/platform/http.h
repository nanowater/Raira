#pragma once

#include <functional>
#include <memory>
#include <string>

/* Minimal blocking HTTP client the app core needs. The simulator implements it with libcurl
 * (src/platform_sim); the ESP32 build implements it with esp_http_client (same interface).
 * POST/PUT may carry a small body (the host bridge takes JSON); the tidal-hifi player API takes its
 * parameters in the query string and sends none. */
struct HttpRequest {
    std::string method = "GET"; /* GET, POST, PUT */
    std::string url;
    std::string user_agent;     /* empty = implementation default */
    std::string body;           /* request body for POST/PUT, empty = none */
    std::string content_type;   /* e.g. "application/json"; only used with a body */
    int connect_timeout_s = 2;
    int timeout_s         = 5;
    std::function<bool()> cancelled; /* polled while transferring; returning true aborts the request */
};

struct HttpResponse {
    bool transport_ok = false; /* false: DNS/connect/timeout/aborted */
    long status       = 0;
    std::string body;
    bool ok() const { return transport_ok && status >= 200 && status < 300; }
};

/* One instance per thread. It may keep connections open between calls (keep-alive). */
class HttpClient {
public:
    virtual ~HttpClient()                            = default;
    virtual HttpResponse perform(const HttpRequest & req) = 0;
};

/* Provided by the platform layer. */
std::unique_ptr<HttpClient> http_client_create();

/* Percent-encode for a URL query component (portable, in src/http_util.cpp). */
std::string url_escape(const std::string & s);
