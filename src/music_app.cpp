#include "music_app.h"
#include "media_poller.h"
#include "music_ui.h"

void music_app_start(const TidalConfig & cfg)
{
    music_ui_create();
    media_poller_start(cfg);
}
