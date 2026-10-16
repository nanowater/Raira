#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include "cover_decoder.h"
#include "media_info.h"

/* What a music service looks like to the app. The core (poller, UI) only talks to this interface, so
 * a new service (Spotify, Apple Music, YouTube Music, a browser companion, a host-side bridge that
 * normalises several of them) is one new class — no change to the UI or the poller.
 *
 * All calls are blocking and are made from the poller thread only (never the LVGL thread), one at a
 * time: implementations need no internal locking. Return false when the service is unreachable or
 * refused; the poller counts consecutive get_current() failures for the "connection lost" banner. */

/* Optional features. A source reports what it supports so the UI can hide the rest later (not wired
 * into the UI yet: every control is shown). */
enum MediaCapability : uint32_t {
    MEDIA_CAP_SEEK     = 1u << 0,
    MEDIA_CAP_VOLUME   = 1u << 1,
    MEDIA_CAP_SHUFFLE  = 1u << 2,
    MEDIA_CAP_REPEAT   = 1u << 3,
    MEDIA_CAP_PREV_NEXT = 1u << 4,
    MEDIA_CAP_BROWSE   = 1u << 5, /* playlists + play queue can be listed (needs a BrowseSource too) */
};

/* One player a service (the host bridge) can control, e.g. "Spotify" or "tidal-hifi" on a PC. */
struct PlayerInfo {
    std::string id;      /* what select/set_player take */
    std::string name;    /* for display */
    std::string service; /* short key: spotify, tidal, browser, other */
    std::string status;  /* playing / paused / stopped */
};

class MediaSource {
public:
    virtual ~MediaSource() = default;

    /* Short name for logs/settings, e.g. "tidal". */
    virtual const char * name() const = 0;
    virtual uint32_t capabilities() const = 0;

    /* Now playing. Fills only what the service knows; see media_info.h. */
    virtual bool get_current(MediaInfo & out) = 0;

    /* Encoded cover (JPEG/PNG bytes) for `info`. `want_px` is the edge length the UI would like at
     * least, so the source can pick a suitable size from a CDN and avoid downloading more than needed. */
    virtual bool get_cover(const MediaInfo & info, int want_px, std::vector<unsigned char> & out) = 0;

    /* Transport. Each returns true when the service accepted the command. */
    virtual bool play_pause()                    = 0;
    virtual bool next()                          = 0;
    virtual bool previous()                      = 0;
    virtual bool toggle_shuffle()                = 0;
    virtual bool toggle_repeat()                 = 0; /* cycles off -> all -> single */
    virtual bool set_volume(double level)        = 0; /* 0.0 .. 1.0 */
    virtual bool seek_absolute(int seconds)      = 0;

    /* Point the source at another server/endpoint ("host[:port]" for a host-side service). Returns false
     * for a malformed value or when the source has no endpoint to configure. Called on the poller thread. */
    virtual bool set_endpoint(const std::string & spec) { (void)spec; return false; }
    virtual std::string endpoint() const { return ""; } /* current endpoint, "" when not applicable */

    /* An access token the service wants with every request (the host bridge outside the loopback). The settings
     * page only offers the token row when uses_token() is true. set_token() is called on the poller thread. */
    /* Which player to show and control. The choice belongs to the device (so several devices do not fight over a
     * shared "current player"): it is sent with every request. "" = automatic, the service decides (what plays).
     * list_players() fills the players on offer and the service's own current pick; set_player() is called on the
     * poller thread, like the other calls. Only services with several players implement them. */
    virtual bool can_select_player() const { return false; }
    virtual bool list_players(std::vector<PlayerInfo> & out, std::string & service_pick) { (void)out; (void)service_pick; return false; }
    virtual void set_player(const std::string & id) { (void)id; }

    virtual bool uses_token() const { return false; }
    virtual void set_token(const std::string & token) { (void)token; }
};

/* ---- Browsing: playlists and the play queue ----
 * Separate from MediaSource on purpose: listing can be slow (a web API behind the host bridge), so it runs on
 * its own worker thread (browse_worker.cpp) and never delays the now-playing poll. Used from that thread
 * only, one call at a time. All calls block; return false on failure. */
struct BrowseItem {
    std::string id;       /* what the service calls it (a URI); a playlist's id is its context */
    std::string context;  /* the playlist this track plays from, "" when unknown (then it is not playable) */
    int index = -1;       /* position within `context` */
    std::string title;
    std::string subtitle; /* artists, track count, ... */
    std::string cover;    /* opaque key for get_thumb(), "" = none */
    bool playable = false;
    bool current  = false; /* the track playing now */
};

class BrowseSource {
public:
    virtual ~BrowseSource() = default;

    /* The fake provider is not tied to the player: its browse button is always shown. */
    virtual bool always_available() const { return false; }

    virtual bool list_queue(std::vector<BrowseItem> & out)     = 0; /* now playing first, then what is next */
    virtual bool list_playlists(std::vector<BrowseItem> & out) = 0;
    virtual bool list_tracks(const std::string & playlist_id, std::string & playlist_title, std::vector<BrowseItem> & out) = 0;
    virtual bool play_item(const BrowseItem & item) = 0; /* start item.context at item.index */
    /* Small cover (about `px` square) decoded to pixels. */
    virtual bool get_thumb(const std::string & cover_key, int px, CoverPixels & out) = 0;

    virtual bool set_endpoint(const std::string & spec) { (void)spec; return false; }
    virtual void set_token(const std::string & token) { (void)token; }
};
