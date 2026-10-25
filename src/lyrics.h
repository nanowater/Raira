#pragma once

#include <string>
#include <vector>

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
     * finding nothing, so asking again later may succeed; false = there are no lyrics for this track. Set by the
     * source's LyricsProvider (the hub: from the PC's answer). */
    bool failed = false;
};
