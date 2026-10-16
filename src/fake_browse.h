#pragma once

#include <memory>
#include "media_source.h"

/* Simulator-only stand-in for a real playlist/queue provider (see bridge/ and docs/BRIDGE_PROTOCOL.md): canned
 * playlists and a queue that follows what the user taps, with generated cover thumbnails. It does not touch the
 * real player and its browse button is always shown. */
std::unique_ptr<BrowseSource> fake_browse_create();
