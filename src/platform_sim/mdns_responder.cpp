#include "platform/mdns.h"
#include <arpa/inet.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <ifaddrs.h>
#include <mutex>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

/* A minimal mDNS responder (RFC 6762) for one service: it answers PTR/SRV/TXT/A questions for it and announces on
 * start and goodbye on stop. No probing or conflict handling (a duplicate name is the user's problem), no known-answer
 * suppression. Answers go out on the interface the question came in on, carrying that interface's address. */

namespace {

const char * GROUP = "224.0.0.251";
const uint16_t PORT = 5353;
const uint32_t TTL = 120;

enum : uint16_t { T_A = 1, T_PTR = 12, T_TXT = 16, T_SRV = 33, T_ANY = 255 };

struct Iface {
    std::string name;
    unsigned index = 0;
    in_addr addr{};
};

std::string lower(std::string s)
{
    for(char & c : s) c = (char)tolower((unsigned char)c);
    return s;
}

/* "a.b.c" -> length-prefixed labels, no compression */
void put_name(std::string & out, const std::string & name)
{
    size_t pos = 0;
    while(pos <= name.size()) {
        size_t dot = name.find('.', pos);
        if(dot == std::string::npos) dot = name.size();
        const size_t n = dot - pos;
        if(n > 0) {
            out += (char)(n > 63 ? 63 : n);
            out.append(name, pos, n > 63 ? 63 : n);
        }
        pos = dot + 1;
        if(dot == name.size()) break;
    }
    out += '\0';
}

void put16(std::string & o, uint16_t v) { o += (char)(v >> 8); o += (char)v; }
void put32(std::string & o, uint32_t v) { put16(o, v >> 16); put16(o, v & 0xFFFF); }

/* Reads a possibly compressed name starting at `pos`; advances `pos` past it in the *original* position. */
bool read_name(const uint8_t * p, size_t len, size_t & pos, std::string & out)
{
    out.clear();
    size_t cur = pos;
    bool jumped = false;
    int hops    = 0;
    while(true) {
        if(cur >= len) return false;
        const uint8_t l = p[cur];
        if(l == 0) {
            if(!jumped) pos = cur + 1;
            return true;
        }
        if((l & 0xC0) == 0xC0) {
            if(cur + 1 >= len || ++hops > 16) return false;
            const size_t target = ((l & 0x3F) << 8) | p[cur + 1];
            if(!jumped) pos = cur + 2;
            jumped = true;
            cur    = target;
            continue;
        }
        if((l & 0xC0) != 0 || cur + 1 + l > len) return false;
        if(!out.empty()) out += '.';
        out.append(reinterpret_cast<const char *>(p + cur + 1), l);
        cur += 1 + l;
    }
}

class Responder : public MdnsAdvertiser {
public:
    ~Responder() override { stop(); }

    bool start(const MdnsService & s) override
    {
        svc_      = s;
        type_fq_  = lower(s.type + ".local");
        inst_fq_  = s.instance + "." + s.type + ".local";
        host_fq_  = lower(s.host + ".local");
        fd_       = socket(AF_INET, SOCK_DGRAM, 0);
        if(fd_ < 0) return false;
        int one = 1;
        setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        setsockopt(fd_, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one)); /* kdeconnect, systemd-resolved... share the port */
        setsockopt(fd_, IPPROTO_IP, IP_PKTINFO, &one, sizeof(one));
        unsigned char ttl = 255, loop = 1;
        setsockopt(fd_, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
        setsockopt(fd_, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop)); /* a source on this same PC must hear us too */
        sockaddr_in a{};
        a.sin_family      = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_ANY);
        a.sin_port        = htons(PORT);
        if(bind(fd_, (sockaddr *)&a, sizeof(a)) != 0) {
            ::close(fd_);
            fd_ = -1;
            return false;
        }
        refresh_interfaces();
        if(joined_.empty()) {
            ::close(fd_);
            fd_ = -1;
            return false;
        }
        running_ = true;
        thread_  = std::thread([this] { loop_(); });
        return true;
    }

    void stop() override
    {
        if(!running_.exchange(false)) return;
        announce(0); /* goodbye */
        if(thread_.joinable()) thread_.join();
        if(fd_ >= 0) ::close(fd_);
        fd_ = -1;
    }

private:
    static std::vector<Iface> list_interfaces()
    {
        std::vector<Iface> out;
        ifaddrs * all = nullptr;
        if(getifaddrs(&all) != 0) return out;
        for(ifaddrs * i = all; i; i = i->ifa_next) {
            if(!i->ifa_addr || i->ifa_addr->sa_family != AF_INET) continue;
            const unsigned f = i->ifa_flags;
            if(!(f & IFF_UP) || !(f & IFF_RUNNING) || !(f & IFF_MULTICAST) || (f & IFF_LOOPBACK)) continue;
            Iface e;
            e.name  = i->ifa_name;
            e.index = if_nametoindex(i->ifa_name);
            e.addr  = reinterpret_cast<sockaddr_in *>(i->ifa_addr)->sin_addr;
            out.push_back(e);
        }
        freeifaddrs(all);
        return out;
    }

    void refresh_interfaces()
    {
        std::lock_guard<std::mutex> lk(mu_);
        for(const Iface & i : list_interfaces()) {
            bool known = false;
            for(const Iface & j : joined_) known = known || (j.index == i.index && j.addr.s_addr == i.addr.s_addr);
            if(known) continue;
            ip_mreq m{};
            inet_pton(AF_INET, GROUP, &m.imr_multiaddr);
            m.imr_interface = i.addr;
            setsockopt(fd_, IPPROTO_IP, IP_ADD_MEMBERSHIP, &m, sizeof(m)); /* EADDRINUSE when already a member: fine */
            joined_.push_back(i);
        }
    }

    /* The whole record set of our service, with `addr` as the host's address. ttl 0 = goodbye. */
    std::string records(in_addr addr, uint32_t ttl, bool ptr, bool srv, bool txt, bool a, bool enumeration, uint16_t & count)
    {
        std::string o;
        count = 0;
        auto rr = [&](const std::string & name, uint16_t type, bool unique, const std::string & rdata) {
            put_name(o, name);
            put16(o, type);
            put16(o, 1 | (unique ? 0x8000 : 0));
            put32(o, ttl);
            put16(o, (uint16_t)rdata.size());
            o += rdata;
            count++;
        };
        if(enumeration) {
            std::string r;
            put_name(r, svc_.type + ".local");
            rr("_services._dns-sd._udp.local", T_PTR, false, r);
        }
        if(ptr) {
            std::string r;
            put_name(r, inst_fq_);
            rr(svc_.type + ".local", T_PTR, false, r);
        }
        if(srv) {
            std::string r;
            put16(r, 0);
            put16(r, 0);
            put16(r, svc_.port);
            put_name(r, svc_.host + ".local");
            rr(inst_fq_, T_SRV, true, r);
        }
        if(txt) {
            std::string r;
            for(const auto & kv : svc_.txt) {
                const std::string e = kv.first + "=" + kv.second;
                r += (char)(e.size() > 255 ? 255 : e.size());
                r += e.substr(0, 255);
            }
            if(r.empty()) r += '\0';
            rr(inst_fq_, T_TXT, true, r);
        }
        if(a) rr(svc_.host + ".local", T_A, true, std::string(reinterpret_cast<const char *>(&addr), 4));
        return o;
    }

    bool send_on(const Iface & i, const std::string & packet, const sockaddr_in * unicast_to)
    {
        std::lock_guard<std::mutex> lk(send_mu_);
        setsockopt(fd_, IPPROTO_IP, IP_MULTICAST_IF, &i.addr, sizeof(i.addr));
        sockaddr_in to{};
        if(unicast_to) to = *unicast_to;
        else {
            to.sin_family = AF_INET;
            to.sin_port   = htons(PORT);
            inet_pton(AF_INET, GROUP, &to.sin_addr);
        }
        return sendto(fd_, packet.data(), packet.size(), 0, (sockaddr *)&to, sizeof(to)) >= 0;
    }

    std::string response(const Iface & i, uint32_t ttl, bool ptr, bool srv, bool txt, bool a, bool enumeration)
    {
        uint16_t n = 0;
        std::string body = records(i.addr, ttl, ptr, srv, txt, a, enumeration, n);
        std::string h;
        put16(h, 0);      /* id */
        put16(h, 0x8400); /* response, authoritative */
        put16(h, 0);
        put16(h, n);
        put16(h, 0);
        put16(h, 0);
        return h + body;
    }

    void announce(uint32_t ttl)
    {
        std::vector<Iface> ifs;
        {
            std::lock_guard<std::mutex> lk(mu_);
            ifs = joined_;
        }
        for(const Iface & i : ifs) send_on(i, response(i, ttl, true, true, true, true, true), nullptr);
    }

    void handle(const uint8_t * p, size_t len, const sockaddr_in & from, unsigned ifindex)
    {
        if(len < 12 || (p[2] & 0x80)) return; /* too short, or a response (not a question) */
        const uint16_t qd = (uint16_t)(p[4] << 8 | p[5]);
        size_t pos        = 12;
        bool ptr = false, srv = false, txt = false, a = false, enumeration = false, unicast = false;
        for(uint16_t q = 0; q < qd; q++) {
            std::string name;
            if(!read_name(p, len, pos, name) || pos + 4 > len) return;
            const uint16_t type  = (uint16_t)(p[pos] << 8 | p[pos + 1]);
            const uint16_t klass = (uint16_t)(p[pos + 2] << 8 | p[pos + 3]);
            pos += 4;
            if(klass & 0x8000) unicast = true; /* QU: the asker wants a unicast answer */
            const std::string n = lower(name);
            const bool any      = type == T_ANY;
            if(n == type_fq_ && (any || type == T_PTR)) { ptr = srv = txt = a = true; }
            else if(n == lower(inst_fq_) && (any || type == T_SRV || type == T_TXT)) {
                srv = any || type == T_SRV;
                txt = any || type == T_TXT;
                a   = true;
            } else if(n == host_fq_ && (any || type == T_A)) a = true;
            else if(n == "_services._dns-sd._udp.local" && (any || type == T_PTR)) enumeration = true;
        }
        if(!(ptr || srv || txt || a || enumeration)) return;

        Iface via;
        bool found = false;
        {
            std::lock_guard<std::mutex> lk(mu_);
            for(const Iface & i : joined_)
                if(i.index == ifindex) { via = i; found = true; break; }
            if(!found && !joined_.empty()) { via = joined_.front(); found = true; }
        }
        if(!found) return;
        const std::string pkt = response(via, TTL, ptr, srv, txt, a, enumeration);
        send_on(via, pkt, nullptr);
        if(unicast) send_on(via, pkt, &from);
    }

    void loop_()
    {
        for(int i = 0; i < 2 && running_; i++) { /* announce twice, a second apart (RFC 6762 §8.3) */
            announce(TTL);
            for(int w = 0; w < 10 && running_; w++) usleep(100 * 1000);
        }
        int idle = 0;
        while(running_) {
            pollfd pf{fd_, POLLIN, 0};
            if(poll(&pf, 1, 500) > 0 && (pf.revents & POLLIN)) {
                uint8_t buf[9000];
                alignas(cmsghdr) char ctrl[256];
                sockaddr_in from{};
                iovec iov{buf, sizeof(buf)};
                msghdr m{};
                m.msg_name       = &from;
                m.msg_namelen    = sizeof(from);
                m.msg_iov        = &iov;
                m.msg_iovlen     = 1;
                m.msg_control    = ctrl;
                m.msg_controllen = sizeof(ctrl);
                const ssize_t n = recvmsg(fd_, &m, 0);
                if(n > 0) {
                    unsigned ifindex = 0;
                    for(cmsghdr * c = CMSG_FIRSTHDR(&m); c; c = CMSG_NXTHDR(&m, c))
                        if(c->cmsg_level == IPPROTO_IP && c->cmsg_type == IP_PKTINFO) ifindex = reinterpret_cast<in_pktinfo *>(CMSG_DATA(c))->ipi_ifindex;
                    handle(buf, (size_t)n, from, ifindex);
                }
            }
            if(++idle >= 20) { /* about every 10 s: pick up interfaces that came up later */
                idle = 0;
                refresh_interfaces();
            }
        }
    }

    MdnsService svc_;
    std::string type_fq_, inst_fq_, host_fq_;
    int fd_ = -1;
    std::atomic<bool> running_{false};
    std::thread thread_;
    std::mutex mu_, send_mu_;
    std::vector<Iface> joined_;
};

} // namespace

std::unique_ptr<MdnsAdvertiser> mdns_advertiser_create()
{
    return std::make_unique<Responder>();
}
