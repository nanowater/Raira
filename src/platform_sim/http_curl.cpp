#include "platform/http.h"
#include <curl/curl.h>

namespace {

size_t write_cb(void * ptr, size_t size, size_t nmemb, void * stream)
{
    static_cast<std::string *>(stream)->append(static_cast<char *>(ptr), size * nmemb);
    return size * nmemb;
}

int xfer_cb(void * clientp, curl_off_t, curl_off_t, curl_off_t, curl_off_t)
{
    const auto * c = static_cast<const std::function<bool()> *>(clientp);
    return (c && *c && (*c)()) ? 1 : 0; /* non-zero aborts the transfer */
}

class CurlClient : public HttpClient {
  public:
    ~CurlClient() override
    {
        if(curl_) curl_easy_cleanup(curl_);
    }

    HttpResponse perform(const HttpRequest & req) override
    {
        HttpResponse res;
        if(!curl_) curl_ = curl_easy_init();
        if(!curl_) return res;
        curl_easy_reset(curl_); /* keeps the open connections for the next request (keep-alive) */

        curl_easy_setopt(curl_, CURLOPT_URL, req.url.c_str());
        curl_easy_setopt(curl_, CURLOPT_WRITEFUNCTION, write_cb);
        curl_easy_setopt(curl_, CURLOPT_WRITEDATA, &res.body);
        curl_easy_setopt(curl_, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl_, CURLOPT_CONNECTTIMEOUT, (long)req.connect_timeout_s);
        curl_easy_setopt(curl_, CURLOPT_TIMEOUT, (long)req.timeout_s);
        curl_easy_setopt(curl_, CURLOPT_NOSIGNAL, 1L);
        if(!req.user_agent.empty()) curl_easy_setopt(curl_, CURLOPT_USERAGENT, req.user_agent.c_str());
        curl_slist * headers = nullptr;
        if(req.method != "GET") { /* POST/PUT, with or without a body */
            curl_easy_setopt(curl_, CURLOPT_POST, 1L);
            curl_easy_setopt(curl_, CURLOPT_POSTFIELDS, req.body.c_str()); /* req outlives perform() */
            curl_easy_setopt(curl_, CURLOPT_POSTFIELDSIZE, (long)req.body.size());
            curl_easy_setopt(curl_, CURLOPT_CUSTOMREQUEST, req.method.c_str());
            if(!req.body.empty() && !req.content_type.empty())
                headers = curl_slist_append(headers, ("Content-Type: " + req.content_type).c_str());
        }
        if(!req.bearer_token.empty())
            headers = curl_slist_append(headers, ("Authorization: Bearer " + req.bearer_token).c_str());
        if(headers) curl_easy_setopt(curl_, CURLOPT_HTTPHEADER, headers);
        if(req.cancelled) {
            curl_easy_setopt(curl_, CURLOPT_NOPROGRESS, 0L);
            curl_easy_setopt(curl_, CURLOPT_XFERINFOFUNCTION, xfer_cb);
            curl_easy_setopt(curl_, CURLOPT_XFERINFODATA, &req.cancelled);
        }

        CURLcode rc = curl_easy_perform(curl_);
        long code   = 0;
        curl_easy_getinfo(curl_, CURLINFO_RESPONSE_CODE, &code);
        curl_slist_free_all(headers);
        res.transport_ok = (rc == CURLE_OK);
        res.status       = code;
        return res;
    }

  private:
    CURL * curl_ = nullptr;
};

} // namespace

std::unique_ptr<HttpClient> http_client_create()
{
    return std::make_unique<CurlClient>();
}
