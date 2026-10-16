#pragma once

#include <chrono>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <nlohmann/json_fwd.hpp>
#include <set>
#include <string>
#include <vector>
#include "media_source.h"
#include "platform/ws_server.h"

/* The hub (docs/HUB_PROTOCOL.md): sources (the PC bridge) connect to this device over WebSocket, pair once with a code
 * shown here, push their players' state and answer commands/cover requests over the same socket. HubSource presents all of
 * that to the rest of the app as one MediaSource, so the poller and the UI stay as they are: the poller "polls" it every
 * 500 ms, which only reads the last pushed state (the position interpolated).
 *
 * Threads: the WebSocket server thread calls the handlers, the poller thread calls the MediaSource methods and the LVGL
 * thread uses the pairing/sources calls below; everything is guarded by one mutex. */

struct HubIdentity {
    std::string device_id;   /* stable, generated once */
    std::string device_name; /* what the user sees in a source's device list */
};

struct PairedSource {
    std::string id;
    std::string name;
    std::string secret; /* hex, 128 bit */
};

struct HubSourceInfo {
    std::string id;
    std::string name;
    std::string os;
    bool paired    = false;
    bool connected = false;
};

class HubSource : public MediaSource {
public:
    static constexpr uint16_t DEFAULT_PORT = 47900;

    HubSource(std::unique_ptr<WsServer> ws, HubIdentity identity, std::vector<PairedSource> paired);
    ~HubSource() override;

    bool start(uint16_t port); /* false: the port is taken */
    /* Called (on the server thread) when the paired list changed: persist it. */
    void set_paired_changed_cb(std::function<void(const std::vector<PairedSource> &)> cb);

    /* ---- for the settings UI (any thread) ---- */
    std::string start_pairing();       /* opens the pairing window (2 min), returns the 6-digit code */
    void stop_pairing();
    std::string pairing_code();        /* "" when the window is closed */
    int pairing_seconds_left();
    std::vector<HubSourceInfo> sources();
    void remove_source(const std::string & id); /* forget the secret and drop the connection */

    /* ---- MediaSource ---- */
    const char * name() const override { return "hub"; }
    uint32_t capabilities() const override;
    bool get_current(MediaInfo & out) override;
    bool get_cover(const MediaInfo & info, int want_px, std::vector<unsigned char> & out) override;
    bool play_pause() override { return command("playpause", ""); }
    bool next() override { return command("next", ""); }
    bool previous() override { return command("previous", ""); }
    bool toggle_shuffle() override;
    bool toggle_repeat() override;
    bool set_volume(double level) override;
    bool seek_absolute(int seconds) override;

    LyricsProvider lyrics_provider() override
    {
        return [this](const MediaInfo & info, Lyrics & out, const std::function<bool()> & cancelled) { return request_lyrics(info, out, cancelled); };
    }

    bool can_select_player() const override { return true; }
    bool list_players(std::vector<PlayerInfo> & out, std::string & service_pick) override;
    void set_player(const std::string & id) override;

private:
    struct Player {
        std::string id, name, service, status;
        bool has_state = false;
        MediaInfo info;     /* position in info.current_sec is only a base: see position_ms */
        int64_t position_ms = 0;
        double rate         = 1.0;
        std::chrono::steady_clock::time_point arrival;
        std::chrono::steady_clock::time_point playing_since;
        bool playing = false;
        std::string cover;  /* source's cover key */
    };
    struct Src {
        std::string id, name, os;
        bool ready = false; /* paired/authenticated */
        std::set<std::string> features;
        std::vector<std::string> order; /* player ids in the order the source listed them */
        std::string selected;           /* the source's own pick (the PC's settings page, else what plays there): "" = none sent */
        std::map<std::string, Player> players;
        std::string last_focus_sent = "?"; /* what we last told this source */
        int auth_failures = 0;
    };
    struct LyricsWait {
        bool done = false, found = false;
        Lyrics lyrics;
    };
    struct CoverWait {
        bool done = false, ok = false;
        std::string data;
    };

    bool on_open(WsConnId, const std::string & path);
    void on_text(WsConnId, const std::string & text);
    void on_binary(WsConnId, const std::string & data);
    void on_close(WsConnId);
    void handle_hello(WsConnId, Src &, const nlohmann::json &);
    void handle_state(Src &, const nlohmann::json &);
    void handle_players(Src &, const nlohmann::json &);
    std::string new_secret();
    std::string key_of(const Src & s, const Player & p) const { return s.id + "/" + p.id; }
    /* The (conn, source, player) shown now, or nulls. Needs mu_. */
    bool focus_locked(WsConnId & conn, Src *& src, Player *& pl, std::string & key);
    /* The focused source looks the lyrics up (it announced the "lyrics" feature), else we fetch from LRCLIB ourselves. */
    bool request_lyrics(const MediaInfo & info, Lyrics & out, const std::function<bool()> & cancelled);
    bool command(const char * op, const std::string & args_json); /* args_json: a JSON object ("" = none) */
    bool send_json(WsConnId id, const std::string & text);

    std::unique_ptr<WsServer> ws_;
    HubIdentity identity_;
    std::vector<PairedSource> paired_;
    std::function<void(const std::vector<PairedSource> &)> paired_cb_;

    std::mutex mu_;
    std::condition_variable cv_;
    std::map<WsConnId, Src> srcs_;
    std::string focus_pref_;  /* "" = automatic, else "<source id>/<player id>" */
    std::string auto_focus_;  /* what automatic chose last (kept while nothing else starts playing) */
    uint32_t next_id_ = 1;
    std::map<uint32_t, CoverWait> cover_waits_;
    std::map<uint32_t, LyricsWait> lyrics_waits_;

    std::string pair_code_;
    std::chrono::steady_clock::time_point pair_until_;
    int pair_failures_ = 0;
};
