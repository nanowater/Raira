#pragma once

#include <cstdint>
#include <vector>
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
};
