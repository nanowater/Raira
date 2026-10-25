#pragma once

#include <functional>
#include <string>
#include <vector>
#include "media_info.h"

struct LyricLine
{
    int time_ms = 0; /* 0 for unsynced lyrics */
    std::string text;
};

struct Lyrics
{
    bool synced = false; /* true: lines carry timestamps, false: plain text */
    std::vector<LyricLine> lines;
    /* Only meaningful when a lookup returned false: true = it failed (network, server, no answer in time) instead of
     * finding nothing, so asking again later may succeed; false = there are no lyrics for this track. Set by the lookup
     * functions, not by the parser. */
    bool failed = false;
};

/* Parse LRC ("[mm:ss.xx]text", several stamps per line allowed). Timestamped lines with no text
 * are kept (empty `text`): they mark instrumental gaps, where no line should be highlighted. */
Lyrics lyrics_parse_lrc(const std::string & lrc);
Lyrics lyrics_parse_plain(const std::string & text);

/* Blocking lookup on LRCLIB (https://lrclib.net). Never call from the LVGL thread.
 * Returns false when nothing was found, the request failed, or `cancelled` turned true
 * (checked between requests and during transfers, so a superseded lookup stops quickly). `out.failed` tells a failure
 * from a track without lyrics: a request that got no usable answer (anything but 200 and 404) while there is no
 * candidate at all. */
bool lyrics_fetch(const MediaInfo & info, Lyrics & out, const std::function<bool()> & cancelled = nullptr);
