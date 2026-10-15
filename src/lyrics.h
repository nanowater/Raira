#pragma once

#include <string>
#include <vector>
#include "media_info.h"

struct LyricLine {
    int time_ms = 0; /* 0 for unsynced lyrics */
    std::string text;
};

struct Lyrics {
    bool synced = false; /* true: lines carry timestamps, false: plain text */
    std::vector<LyricLine> lines;
};

/* Parse LRC ("[mm:ss.xx]text", several stamps per line allowed). Empty lines are dropped. */
Lyrics lyrics_parse_lrc(const std::string & lrc);
Lyrics lyrics_parse_plain(const std::string & text);

/* Blocking lookup on LRCLIB (https://lrclib.net). Never call from the LVGL thread.
 * Returns false when nothing was found or the request failed. */
bool lyrics_fetch(const MediaInfo & info, Lyrics & out);
