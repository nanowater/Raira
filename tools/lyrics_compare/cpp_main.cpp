// Prints what the device's own lyrics code (src/lyrics.cpp) finds, in the same JSON form as `music-bridge
// --lyrics-test`. Usage: cpp_lyrics <title> <artists> <album> <seconds>
#include <cstdio>
#include <cstdlib>
#include <nlohmann/json.hpp>
#include "lyrics.h"

int main(int argc, char ** argv)
{
    if(argc < 5) return 2;
    MediaInfo info;
    info.title        = argv[1];
    info.artists      = argv[2];
    info.album        = argv[3];
    info.duration_sec = atoi(argv[4]);
    Lyrics l;
    if(!lyrics_fetch(info, l)) {
        printf("{\"found\":false}\n");
        return 0;
    }
    nlohmann::json j = {{"synced", l.synced}, {"lines", nlohmann::json::array()}};
    for(const auto & x : l.lines) j["lines"].push_back({x.time_ms, x.text});
    printf("%s\n", j.dump().c_str());
}
