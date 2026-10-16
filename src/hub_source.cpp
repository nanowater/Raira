#include "hub_source.h"
#include <algorithm>
#include <cstdio>
#include <nlohmann/json.hpp>
#include <random>

using nlohmann::json;
using clk = std::chrono::steady_clock;

namespace {

const std::chrono::seconds PAIR_WINDOW(120);
const int PAIR_MAX_FAILURES = 3;
const int AUTH_MAX_FAILURES = 3;
const std::chrono::seconds COVER_TIMEOUT(8);
const size_t MAX_TEXT = 16 * 1024 + 64 * 1024; /* lyrics frames are the big ones; plain messages are far smaller */

bool same(const std::string & a, const std::string & b)
{
    return a.size() == b.size() && [&] {
        unsigned char acc = 0;
        for(size_t i = 0; i < a.size(); i++) acc |= (unsigned char)(a[i] ^ b[i]);
        return acc == 0;
    }();
}

std::string random_hex(size_t bytes)
{
    static const char H[] = "0123456789abcdef";
    std::random_device rd;
    std::string out;
    for(size_t i = 0; i < bytes; i++) {
        unsigned v = rd() & 0xFF;
        out += H[v >> 4];
        out += H[v & 15];
    }
    return out;
}

std::string str_of(const json & j, const char * key)
{
    auto it = j.find(key);
    return (it != j.end() && it->is_string()) ? it->get<std::string>() : std::string();
}

uint32_t caps_of(const json & j)
{
    uint32_t caps = 0;
    auto it       = j.find("capabilities");
    if(it == j.end() || !it->is_array()) return caps;
    for(const auto & c : *it) {
        if(!c.is_string()) continue;
        const std::string s = c.get<std::string>();
        if(s == "seek") caps |= MEDIA_CAP_SEEK;
        else if(s == "volume") caps |= MEDIA_CAP_VOLUME;
        else if(s == "shuffle") caps |= MEDIA_CAP_SHUFFLE;
        else if(s == "repeat") caps |= MEDIA_CAP_REPEAT;
        else if(s == "prev_next") caps |= MEDIA_CAP_PREV_NEXT;
        else if(s == "browse_playlists" || s == "browse_queue") caps |= MEDIA_CAP_BROWSE;
    }
    return caps;
}

} // namespace

HubSource::HubSource(std::unique_ptr<WsServer> ws, HubIdentity identity, std::vector<PairedSource> paired)
    : ws_(std::move(ws)), identity_(std::move(identity)), paired_(std::move(paired))
{
}

HubSource::~HubSource()
{
    if(ws_) ws_->stop();
}

bool HubSource::start(uint16_t port)
{
    WsHandlers h;
    h.on_open   = [this](WsConnId id, const std::string & path) { return on_open(id, path); };
    h.on_text   = [this](WsConnId id, const std::string & t) { on_text(id, t); };
    h.on_binary = [this](WsConnId id, const std::string & d) { on_binary(id, d); };
    h.on_close  = [this](WsConnId id) { on_close(id); };
    WsLimits limits;
    limits.max_connections = 4; /* docs/HUB_PROTOCOL.md: at most 4 sources */
    limits.max_message     = 256 * 1024;
    return ws_->start(port, std::move(h), limits);
}

void HubSource::set_paired_changed_cb(std::function<void(const std::vector<PairedSource> &)> cb)
{
    std::lock_guard<std::mutex> lk(mu_);
    paired_cb_ = std::move(cb);
}

uint32_t HubSource::capabilities() const
{
    return MEDIA_CAP_SEEK | MEDIA_CAP_VOLUME | MEDIA_CAP_SHUFFLE | MEDIA_CAP_REPEAT | MEDIA_CAP_PREV_NEXT;
}

bool HubSource::send_json(WsConnId id, const std::string & text)
{
    return ws_->send_text(id, text);
}

/* ---- connection handling (server thread) ---- */

bool HubSource::on_open(WsConnId id, const std::string & path)
{
    if(path != "/source") return false;
    std::lock_guard<std::mutex> lk(mu_);
    srcs_[id] = Src{};
    return true;
}

void HubSource::on_close(WsConnId id)
{
    std::lock_guard<std::mutex> lk(mu_);
    srcs_.erase(id);
    cv_.notify_all(); /* cover waits re-check */
}

void HubSource::handle_hello(WsConnId id, Src & s, const json & j)
{
    s.id   = str_of(j, "source_id");
    s.name = str_of(j, "name");
    s.os   = str_of(j, "os");
    s.features.clear();
    auto f = j.find("features");
    if(f != j.end() && f->is_array())
        for(const auto & x : *f)
            if(x.is_string()) s.features.insert(x.get<std::string>());
    /* a source that reconnects replaces its previous (probably half-dead) connection */
    for(auto & kv : srcs_)
        if(kv.first != id && !s.id.empty() && kv.second.id == s.id) ws_->close(kv.first);

    json w = {{"t", "welcome"}, {"proto", 1}, {"device_id", identity_.device_id}, {"name", identity_.device_name},
              {"max_text", MAX_TEXT}, {"max_binary", 256 * 1024}};
    send_json(id, w.dump());
    const bool known = std::any_of(paired_.begin(), paired_.end(), [&](const PairedSource & p) { return p.id == s.id; });
    if(!known) send_json(id, "{\"t\":\"pairing_required\"}");
}

void HubSource::handle_players(Src & s, const json & j)
{
    auto list = j.find("players");
    if(list == j.end() || !list->is_array()) return;
    std::map<std::string, Player> next;
    std::vector<std::string> order;
    for(const auto & p : *list) {
        if(!p.is_object()) continue;
        const std::string id = str_of(p, "id");
        if(id.empty()) continue;
        Player pl;
        auto old = s.players.find(id);
        if(old != s.players.end()) pl = old->second;
        pl.id      = id;
        pl.name    = str_of(p, "name");
        pl.service = str_of(p, "service");
        pl.status  = str_of(p, "status");
        const bool playing = pl.status == "playing";
        if(playing && !pl.playing) pl.playing_since = clk::now();
        pl.playing = playing;
        order.push_back(id);
        next[id] = std::move(pl);
    }
    s.players  = std::move(next);
    s.order    = std::move(order);
    s.selected = str_of(j, "selected");
}

void HubSource::handle_state(Src & s, const json & j)
{
    auto it = s.players.find(str_of(j, "player"));
    if(it == s.players.end()) return; /* not in the last `players`: ignored */
    Player & p = it->second;

    MediaInfo m;
    m.status = str_of(j, "status");
    auto tr  = j.find("track");
    if(tr != j.end() && tr->is_object()) {
        m.title    = str_of(*tr, "title");
        m.album    = str_of(*tr, "album");
        m.track_id = str_of(*tr, "id");
        p.cover    = str_of(*tr, "cover");
        auto ar    = tr->find("artists");
        if(ar != tr->end() && ar->is_array())
            for(const auto & a : *ar) {
                if(!a.is_string()) continue;
                if(!m.artists.empty()) m.artists += ", ";
                m.artists += a.get<std::string>();
            }
        auto d = tr->find("duration_ms");
        if(d != tr->end() && d->is_number()) m.duration_sec = (int)(d->get<double>() / 1000.0);
    } else {
        p.cover.clear();
    }
    auto pos = j.find("position_ms");
    p.position_ms = (pos != j.end() && pos->is_number()) ? (int64_t)pos->get<double>() : 0;
    auto rate     = j.find("rate");
    p.rate        = (rate != j.end() && rate->is_number()) ? rate->get<double>() : 1.0;
    auto vol      = j.find("volume");
    if(vol != j.end() && vol->is_number()) m.volume = vol->get<double>();
    auto sh = j.find("shuffle");
    if(sh != j.end() && sh->is_boolean()) m.shuffle = sh->get<bool>();
    m.repeat = str_of(j, "repeat");
    auto q   = j.find("quality");
    if(q != j.end() && q->is_object()) {
        m.quality       = str_of(*q, "label");
        m.quality_badge = str_of(*q, "badge");
    }
    m.caps     = caps_of(j);
    p.info     = std::move(m);
    p.arrival  = clk::now();
    p.has_state = true;
    p.status   = p.info.status.empty() ? p.status : p.info.status;
    const bool playing = p.status == "playing";
    if(playing && !p.playing) p.playing_since = clk::now();
    p.playing = playing;
}

std::string HubSource::new_secret()
{
    return random_hex(16);
}

void HubSource::on_text(WsConnId id, const std::string & text)
{
    json j = json::parse(text, nullptr, false);
    if(j.is_discarded() || !j.is_object()) return;
    const std::string t = str_of(j, "t");

    std::vector<PairedSource> paired_copy;
    std::function<void(const std::vector<PairedSource> &)> cb;
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = srcs_.find(id);
        if(it == srcs_.end()) return;
        Src & s = it->second;

        if(t == "hello") {
            handle_hello(id, s, j);
        } else if(t == "auth" && !s.id.empty()) {
            auto p = std::find_if(paired_.begin(), paired_.end(), [&](const PairedSource & x) { return x.id == s.id; });
            if(p != paired_.end() && same(p->secret, str_of(j, "secret"))) {
                s.ready = true;
                send_json(id, "{\"t\":\"ready\"}");
            } else {
                send_json(id, "{\"t\":\"auth_failed\"}");
                if(++s.auth_failures >= AUTH_MAX_FAILURES) ws_->close(id);
            }
        } else if(t == "pair" && !s.id.empty()) {
            const bool open = !pair_code_.empty() && clk::now() < pair_until_;
            if(open && same(pair_code_, str_of(j, "code"))) {
                PairedSource ps{s.id, s.name, new_secret()};
                paired_.erase(std::remove_if(paired_.begin(), paired_.end(), [&](const PairedSource & x) { return x.id == ps.id; }), paired_.end());
                paired_.push_back(ps);
                pair_code_.clear(); /* one pairing per code */
                s.ready = true;
                json r = {{"t", "paired"}, {"secret", ps.secret}, {"device_id", identity_.device_id}, {"name", identity_.device_name}};
                send_json(id, r.dump());
                paired_copy = paired_;
                cb          = paired_cb_;
            } else {
                send_json(id, open ? "{\"t\":\"pair_failed\",\"reason\":\"wrong_code\"}" : "{\"t\":\"pair_failed\",\"reason\":\"closed\"}");
                if(open && ++pair_failures_ >= PAIR_MAX_FAILURES) pair_code_.clear(); /* too many guesses: close the window */
            }
        } else if(s.ready) {
            if(t == "players") handle_players(s, j);
            else if(t == "state") handle_state(s, j);
            else if(t == "cover_err") {
                auto w = cover_waits_.find((uint32_t)j.value("id", 0));
                if(w != cover_waits_.end()) {
                    w->second.done = true;
                    w->second.ok   = false;
                    cv_.notify_all();
                }
            }
            /* "ack" and the rest: nothing to do yet */
        }
    }
    if(cb) cb(paired_copy);
}

void HubSource::on_binary(WsConnId id, const std::string & data)
{
    if(data.size() < 4) return;
    const uint32_t req = (uint32_t)(uint8_t)data[0] << 24 | (uint32_t)(uint8_t)data[1] << 16 | (uint32_t)(uint8_t)data[2] << 8 | (uint8_t)data[3];
    std::lock_guard<std::mutex> lk(mu_);
    auto it = srcs_.find(id);
    if(it == srcs_.end() || !it->second.ready) return;
    auto w = cover_waits_.find(req);
    if(w == cover_waits_.end()) return;
    w->second.data = data.substr(4);
    w->second.done = true;
    w->second.ok   = true;
    cv_.notify_all();
}

/* ---- focus ---- */

bool HubSource::focus_locked(WsConnId & conn, Src *& src, Player *& pl, std::string & key)
{
    auto find_key = [&](const std::string & k) {
        for(auto & kv : srcs_) {
            if(!kv.second.ready) continue;
            for(auto & pp : kv.second.players) {
                if(key_of(kv.second, pp.second) != k) continue;
                conn = kv.first; src = &kv.second; pl = &pp.second; key = k;
                return true;
            }
        }
        return false;
    };
    if(!focus_pref_.empty()) return find_key(focus_pref_); /* pinned: when it is gone there is nothing to show */

    /* automatic: every source proposes the player it would show itself (the one picked on the PC's settings page, else
     * what plays there; a source that sends no pick proposes whatever plays). Of the proposals that are playing, the one
     * that started last wins, so a browser video does not take over from the TIDAL the user picked on the PC. */
    auto proposal = [](Src & s) -> Player * {
        auto it = s.players.find(s.selected);
        return it == s.players.end() ? nullptr : &it->second;
    };
    bool found = false;
    clk::time_point best;
    std::string best_key;
    auto consider = [&](Src & s, Player & p) {
        if(!p.playing || (found && p.playing_since <= best)) return;
        found    = true;
        best     = p.playing_since;
        best_key = key_of(s, p);
    };
    for(auto & kv : srcs_) {
        if(!kv.second.ready) continue;
        if(Player * p = proposal(kv.second)) consider(kv.second, *p);
        else
            for(auto & pp : kv.second.players) consider(kv.second, pp.second);
    }
    if(found) {
        auto_focus_ = best_key;
        return find_key(best_key);
    }
    /* nothing proposed is playing: show the proposal of the source looked at last (else of the first source) */
    const std::string last_source = auto_focus_.substr(0, auto_focus_.find('/'));
    for(int pass = 0; pass < 2; pass++) {
        for(auto & kv : srcs_) {
            Src & s = kv.second;
            if(!s.ready || s.order.empty() || (pass == 0 && s.id != last_source)) continue;
            Player * p = proposal(s);
            if(!p) p = &s.players[s.order.front()];
            auto_focus_ = key_of(s, *p);
            return find_key(auto_focus_);
        }
    }
    auto_focus_.clear();
    return false;
}

/* ---- MediaSource ---- */

bool HubSource::get_current(MediaInfo & out)
{
    std::vector<std::pair<WsConnId, std::string>> focus_msgs;
    MediaInfo m;
    {
        std::lock_guard<std::mutex> lk(mu_);
        bool any_ready = false;
        for(auto & kv : srcs_) any_ready = any_ready || kv.second.ready;
        if(!any_ready) return false; /* no source connected: the "connection lost" banner */

        m.status = "stopped";
        m.caps   = 0;
        WsConnId conn = 0;
        Src * src     = nullptr;
        Player * pl   = nullptr;
        std::string key;
        const bool have = focus_locked(conn, src, pl, key);
        if(have && pl->has_state) {
            m = pl->info;
            int64_t pos = pl->position_ms;
            if(pl->playing)
                pos += (int64_t)(pl->rate * (double)std::chrono::duration_cast<std::chrono::milliseconds>(clk::now() - pl->arrival).count());
            if(m.duration_sec > 0 && pos > (int64_t)m.duration_sec * 1000) pos = (int64_t)m.duration_sec * 1000;
            m.current_sec = (int)(pos / 1000);
            if(!pl->cover.empty()) m.image_url = src->id + "|" + pl->cover;
        }
        /* tell each source which of its players the user is looking at (once per change) */
        for(auto & kv : srcs_) {
            if(!kv.second.ready) continue;
            const std::string want = (have && &kv.second == src) ? pl->id : "";
            if(kv.second.last_focus_sent != want) {
                kv.second.last_focus_sent = want;
                json f = {{"t", "focus"}, {"player", want.empty() ? json(nullptr) : json(want)}};
                focus_msgs.emplace_back(kv.first, f.dump());
            }
        }
    }
    for(auto & fm : focus_msgs) send_json(fm.first, fm.second);
    out = std::move(m);
    return true;
}

bool HubSource::get_cover(const MediaInfo & info, int want_px, std::vector<unsigned char> & out)
{
    const size_t bar = info.image_url.find('|');
    if(bar == std::string::npos) return false;
    const std::string sid = info.image_url.substr(0, bar), key = info.image_url.substr(bar + 1);

    WsConnId conn = 0;
    uint32_t req  = 0;
    {
        std::lock_guard<std::mutex> lk(mu_);
        for(auto & kv : srcs_)
            if(kv.second.ready && kv.second.id == sid) conn = kv.first;
        if(!conn) return false;
        req = next_id_++;
        cover_waits_[req] = CoverWait{};
    }
    json r = {{"t", "cover_req"}, {"id", req}, {"key", key}, {"px", want_px}};
    bool ok = send_json(conn, r.dump());

    std::unique_lock<std::mutex> lk(mu_);
    if(ok) cv_.wait_for(lk, COVER_TIMEOUT, [&] { return cover_waits_[req].done || srcs_.find(conn) == srcs_.end(); });
    CoverWait w = cover_waits_[req];
    cover_waits_.erase(req);
    if(!w.done || !w.ok || w.data.empty()) return false;
    out.assign(w.data.begin(), w.data.end());
    return true;
}

bool HubSource::command(const char * op, const std::string & args_json)
{
    WsConnId conn = 0;
    std::string text;
    {
        std::lock_guard<std::mutex> lk(mu_);
        Src * src = nullptr;
        Player * pl = nullptr;
        std::string key;
        if(!focus_locked(conn, src, pl, key)) return false;
        json c = args_json.empty() ? json::object() : json::parse(args_json, nullptr, false);
        if(c.is_discarded() || !c.is_object()) c = json::object();
        c["t"]      = "cmd";
        c["id"]     = next_id_++;
        c["player"] = pl->id;
        c["op"]     = op;
        text        = c.dump();
    }
    return send_json(conn, text);
}

bool HubSource::toggle_shuffle()
{
    bool now = false;
    {
        std::lock_guard<std::mutex> lk(mu_);
        WsConnId c; Src * s; Player * p; std::string k;
        if(focus_locked(c, s, p, k)) now = p->info.shuffle;
    }
    return command("shuffle", std::string("{\"enabled\":") + (now ? "false" : "true") + "}");
}

bool HubSource::toggle_repeat()
{
    std::string now = "off";
    {
        std::lock_guard<std::mutex> lk(mu_);
        WsConnId c; Src * s; Player * p; std::string k;
        if(focus_locked(c, s, p, k) && !p->info.repeat.empty()) now = p->info.repeat;
    }
    const char * next = now == "off" ? "all" : now == "all" ? "single" : "off";
    return command("repeat", std::string("{\"mode\":\"") + next + "\"}");
}

bool HubSource::set_volume(double level)
{
    if(level < 0) level = 0;
    if(level > 1) level = 1;
    return command("volume", json{{"value", level}}.dump());
}

bool HubSource::seek_absolute(int seconds)
{
    return command("seek", json{{"position_ms", (int64_t)seconds * 1000}}.dump());
}

bool HubSource::list_players(std::vector<PlayerInfo> & out, std::string & service_pick)
{
    std::lock_guard<std::mutex> lk(mu_);
    out.clear();
    for(auto & kv : srcs_) {
        if(!kv.second.ready) continue;
        for(const std::string & pid : kv.second.order) {
            const Player & p = kv.second.players[pid];
            out.push_back({key_of(kv.second, p), kv.second.name + " - " + p.name, p.service, p.status});
        }
    }
    WsConnId c; Src * s; Player * p; std::string k;
    service_pick = (focus_pref_.empty() && focus_locked(c, s, p, k)) ? k : "";
    return true;
}

void HubSource::set_player(const std::string & id)
{
    std::lock_guard<std::mutex> lk(mu_);
    focus_pref_ = id;
}

/* ---- pairing and the sources list (settings UI) ---- */

std::string HubSource::start_pairing()
{
    std::random_device rd;
    char code[8];
    snprintf(code, sizeof(code), "%06u", (unsigned)(rd() % 1000000));
    std::lock_guard<std::mutex> lk(mu_);
    pair_code_      = code;
    pair_until_     = clk::now() + PAIR_WINDOW;
    pair_failures_  = 0;
    return pair_code_;
}

void HubSource::stop_pairing()
{
    std::lock_guard<std::mutex> lk(mu_);
    pair_code_.clear();
}

std::string HubSource::pairing_code()
{
    std::lock_guard<std::mutex> lk(mu_);
    return (!pair_code_.empty() && clk::now() < pair_until_) ? pair_code_ : "";
}

int HubSource::pairing_seconds_left()
{
    std::lock_guard<std::mutex> lk(mu_);
    if(pair_code_.empty() || clk::now() >= pair_until_) return 0;
    return (int)std::chrono::duration_cast<std::chrono::seconds>(pair_until_ - clk::now()).count() + 1;
}

std::vector<HubSourceInfo> HubSource::sources()
{
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<HubSourceInfo> out;
    for(const PairedSource & p : paired_) out.push_back({p.id, p.name, "", true, false});
    for(auto & kv : srcs_) {
        if(kv.second.id.empty()) continue;
        auto it = std::find_if(out.begin(), out.end(), [&](const HubSourceInfo & i) { return i.id == kv.second.id; });
        if(it == out.end()) {
            out.push_back({kv.second.id, kv.second.name, kv.second.os, false, true});
        } else {
            it->connected = kv.second.ready;
            it->os        = kv.second.os;
            if(!kv.second.name.empty()) it->name = kv.second.name;
        }
    }
    return out;
}

void HubSource::remove_source(const std::string & id)
{
    std::vector<PairedSource> copy;
    std::function<void(const std::vector<PairedSource> &)> cb;
    {
        std::lock_guard<std::mutex> lk(mu_);
        paired_.erase(std::remove_if(paired_.begin(), paired_.end(), [&](const PairedSource & p) { return p.id == id; }), paired_.end());
        for(auto & kv : srcs_)
            if(kv.second.id == id) {
                kv.second.ready = false;
                ws_->close(kv.first);
            }
        copy = paired_;
        cb   = paired_cb_;
    }
    if(cb) cb(copy);
}
