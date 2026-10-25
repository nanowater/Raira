#include "platform/ws_server.h"
#include <atomic>
#include <cstring>
#include <deque>
#include <libwebsockets.h>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

/* WsServer on libwebsockets (simulator). lws is single-threaded: only its service thread may touch a connection, so
 * send_*()/close() from other threads queue the data per connection and wake the service thread
 * (lws_cancel_service), which then asks for a writable callback and writes there. */

namespace {

struct Out
{
    enum Kind { TEXT, BINARY, CLOSE } kind;
    std::string data;
};

struct Conn
{
    lws * wsi = nullptr;
    std::string peer;
    std::string rx; /* message being assembled from fragments */
    bool rx_binary = false;
    bool open      = false; /* on_open accepted it */
    std::deque<Out> out;    /* guarded by the server mutex */
};

class LwsWsServer : public WsServer {
  public:
    ~LwsWsServer() override
    {
        stop();
    }

    bool start(uint16_t port, WsHandlers handlers, WsLimits limits) override
    {
        h_      = std::move(handlers);
        limits_ = limits;
        lws_set_log_level(LLL_ERR | LLL_WARN, nullptr);

        static lws_protocols protocols[] = {
            {"musicbridge", &LwsWsServer::lws_cb, 0, 16 * 1024, 0, nullptr, 0},
            LWS_PROTOCOL_LIST_TERM,
        };
        /* liveness: ping idle connections every 10 s, hang up on one silent for 25 s (docs/HUB_PROTOCOL.md) */
        static const lws_retry_bo_t retry = {
            nullptr, 0,  0,
            10,      25, 0}; /* table, count, conceal, secs_since_valid_ping, secs_since_valid_hangup, jitter */

        lws_context_creation_info info;
        memset(&info, 0, sizeof(info));
        info.port                  = port;
        info.protocols             = protocols;
        info.user                  = this;
        info.retry_and_idle_policy = &retry;
        ctx_                       = lws_create_context(&info);
        if(!ctx_) return false;
        running_ = true;
        thread_  = std::thread([this] {
            while(running_) lws_service(ctx_, 0);
        });
        return true;
    }

    void stop() override
    {
        if(!running_.exchange(false)) return;
        lws_cancel_service(ctx_);
        if(thread_.joinable()) thread_.join();
        lws_context_destroy(ctx_);
        ctx_ = nullptr;
        std::lock_guard<std::mutex> lk(mu_);
        conns_.clear();
        by_wsi_.clear();
    }

    bool send_text(WsConnId id, const std::string & text) override
    {
        return enqueue(id, {Out::TEXT, text});
    }
    bool send_binary(WsConnId id, const std::string & data) override
    {
        return enqueue(id, {Out::BINARY, data});
    }
    void close(WsConnId id) override
    {
        enqueue(id, {Out::CLOSE, ""});
    }

    std::string peer(WsConnId id) override
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = conns_.find(id);
        return it == conns_.end() ? "" : it->second->peer;
    }

  private:
    static int lws_cb(lws * wsi, lws_callback_reasons reason, void *, void * in, size_t len)
    {
        auto * self = static_cast<LwsWsServer *>(lws_context_user(lws_get_context(wsi)));
        return self ? self->callback(wsi, reason, in, len) : 0;
    }

    bool enqueue(WsConnId id, Out o)
    {
        {
            std::lock_guard<std::mutex> lk(mu_);
            auto it = conns_.find(id);
            if(it == conns_.end() || !it->second->open) return false;
            size_t queued = 0;
            for(const Out & q : it->second->out) queued += q.data.size();
            if(queued + o.data.size() > 2 * limits_.max_message) return false; /* a peer that does not read */
            it->second->out.push_back(std::move(o));
        }
        if(ctx_) lws_cancel_service(ctx_);
        return true;
    }

    std::shared_ptr<Conn> by_wsi(lws * wsi, WsConnId * id_out = nullptr)
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto w = by_wsi_.find(wsi);
        if(w == by_wsi_.end()) return nullptr;
        if(id_out) *id_out = w->second;
        return conns_[w->second];
    }

    int callback(lws * wsi, lws_callback_reasons reason, void * in, size_t len)
    {
        switch(reason) {
            case LWS_CALLBACK_FILTER_PROTOCOL_CONNECTION: { /* during the handshake: remember the requested path */
                char path[256] = "";
                if(lws_hdr_copy(wsi, path, sizeof(path), WSI_TOKEN_GET_URI) <= 0) path[0] = 0;
                std::lock_guard<std::mutex> lk(mu_);
                pending_path_[wsi] = path;
                return 0;
            }
            case LWS_CALLBACK_ESTABLISHED: {
                std::string path;
                auto c      = std::make_shared<Conn>();
                c->wsi      = wsi;
                char ip[64] = "";
                lws_get_peer_simple(wsi, ip, sizeof(ip));
                c->peer = ip;
                WsConnId id;
                {
                    std::lock_guard<std::mutex> lk(mu_);
                    auto p = pending_path_.find(wsi);
                    if(p != pending_path_.end()) {
                        path = p->second;
                        pending_path_.erase(p);
                    }
                    if(conns_.size() >= limits_.max_connections) return -1;
                    id           = next_id_++;
                    conns_[id]   = c;
                    by_wsi_[wsi] = id;
                }
                const bool ok = !h_.on_open || h_.on_open(id, path);
                if(!ok) return -1; /* on_close is not called for a refused connection: it never opened */
                std::lock_guard<std::mutex> lk(mu_);
                c->open = true;
                return 0;
            }
            case LWS_CALLBACK_RECEIVE: {
                WsConnId id;
                auto c = by_wsi(wsi, &id);
                if(!c) return -1;
                if(lws_is_first_fragment(wsi)) {
                    c->rx.clear();
                    c->rx_binary = lws_frame_is_binary(wsi);
                }
                if(c->rx.size() + len > limits_.max_message) return -1;
                c->rx.append(static_cast<const char *>(in), len);
                if(lws_is_final_fragment(wsi) && lws_remaining_packet_payload(wsi) == 0) {
                    std::string msg = std::move(c->rx);
                    c->rx.clear();
                    if(c->rx_binary) {
                        if(h_.on_binary) h_.on_binary(id, msg);
                    } else if(h_.on_text)
                        h_.on_text(id, msg);
                }
                return 0;
            }
            case LWS_CALLBACK_EVENT_WAIT_CANCELLED: { /* some thread queued data or wants a close */
                std::vector<lws *> todo;
                {
                    std::lock_guard<std::mutex> lk(mu_);
                    for(auto & kv : conns_)
                        if(!kv.second->out.empty()) todo.push_back(kv.second->wsi);
                }
                for(lws * w : todo) lws_callback_on_writable(w);
                return 0;
            }
            case LWS_CALLBACK_SERVER_WRITEABLE: {
                std::shared_ptr<Conn> c = by_wsi(wsi);
                if(!c) return -1;
                Out o;
                bool more = false;
                {
                    std::lock_guard<std::mutex> lk(mu_);
                    if(c->out.empty()) return 0;
                    o = std::move(c->out.front());
                    c->out.pop_front();
                    more = !c->out.empty();
                }
                if(o.kind == Out::CLOSE) {
                    lws_close_reason(wsi, LWS_CLOSE_STATUS_NORMAL, nullptr, 0);
                    return -1;
                }
                std::vector<unsigned char> buf(LWS_PRE + o.data.size());
                memcpy(buf.data() + LWS_PRE, o.data.data(), o.data.size());
                const int n = lws_write(wsi, buf.data() + LWS_PRE, o.data.size(),
                                        o.kind == Out::TEXT ? LWS_WRITE_TEXT : LWS_WRITE_BINARY);
                if(n < 0) return -1;
                if(more) lws_callback_on_writable(wsi); /* one frame per writable callback */
                return 0;
            }
            case LWS_CALLBACK_CLOSED: {
                WsConnId id   = 0;
                bool was_open = false;
                {
                    std::lock_guard<std::mutex> lk(mu_);
                    pending_path_.erase(wsi);
                    auto w = by_wsi_.find(wsi);
                    if(w != by_wsi_.end()) {
                        id      = w->second;
                        auto it = conns_.find(id);
                        if(it != conns_.end()) {
                            was_open = it->second->open;
                            conns_.erase(it);
                        }
                        by_wsi_.erase(w);
                    }
                }
                if(was_open && h_.on_close) h_.on_close(id);
                return 0;
            }
            default: return 0;
        }
    }

    WsHandlers h_;
    WsLimits limits_;
    lws_context * ctx_ = nullptr;
    std::atomic<bool> running_{false};
    std::thread thread_;
    std::mutex mu_;
    std::map<WsConnId, std::shared_ptr<Conn>> conns_;
    std::map<lws *, WsConnId> by_wsi_;
    std::map<lws *, std::string> pending_path_;
    WsConnId next_id_ = 1;
};

} // namespace

std::unique_ptr<WsServer> ws_server_create()
{
    return std::make_unique<LwsWsServer>();
}
