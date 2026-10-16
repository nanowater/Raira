# Host bridge protocol (draft v0)

Status: **draft**. The Linux bridge in `bridge/` (Rust, MPRIS) implements `/info`, `/players`, `/players/select`, `/state`, `/cmd/*` and `/cover/<key>`; browse (`/browse/*`, `/cmd/play_item`) and the TIDAL/Spotify supplements are not implemented, so `quality` is always `null` and the `browse_*` capabilities never appear. **Browse is roadmap only**: it needs the Spotify Web API, whose development mode requires a Premium app owner (see AGENTS.md); an implementation is parked on the branch `spotify-browse`. The device side is `BridgeSource` + `BridgeBrowse` (`lvglsim -P`); `BridgeBrowse` already speaks the browse endpoints below, the bridge does not serve them yet. Written 2026-10-16 from the MPRIS probing described below and from `src/media_info.h` / `src/media_source.h`. Items marked **(unverified)** were not tested.

## Why a bridge

The device is a controller; the host PC plays the audio. Instead of putting per-service OAuth/TLS code on the ESP32, a small **host bridge** talks to the services and exposes one normalised protocol. The device (and the simulator) have a single `MediaSource` implementation, `BridgeSource`, that speaks this protocol. Adding a service changes the bridge only.

```
device / simulator ──WiFi HTTP──▶ host bridge ─┬─ OS media session (Linux MPRIS, Windows GSMTC)  ← base
   BridgeSource                                ├─ Spotify Web API   (playlists, queue)           ← supplement
                                               └─ tidal-hifi API    (shuffle/repeat, quality)   ← supplement
```

Decisions behind this (see also the project notes): OS media sessions are the base for every service, including TIDAL; service APIs only fill what the OS session cannot do. macOS is out of scope for now.

## What the OS session can and cannot do (probed on Linux, 2026-10-16)

| | tidal-hifi (MPRIS) | Spotify desktop (MPRIS) |
|---|---|---|
| Identity | `Identity = "tidal-hifi"`; bus name is `chromium.instance<PID>` (unstable, do not key on it) | `Identity = "Spotify"`, `DesktopEntry = "spotify"`, bus name `spotify` |
| Metadata | title, artist, album, length | + albumArtist, track number, `xesam:url` (`open.spotify.com/track/<id>`) |
| Position | microseconds | microseconds |
| Cover (`mpris:artUrl`) | `file:///tmp/.org.chromium.Chromium.XXXX` — a **host-local temp file** | `https://i.scdn.co/...` |
| Play/pause/next/prev/seek/volume | yes | yes |
| Shuffle / repeat (`Shuffle`, `LoopStatus`) | **absent** | yes (read/write) |
| Audio quality | no | n/a |
| `OpenUri` | not checked | yes: `spotify:track:` and `spotify:playlist:` start playback; `Next` then follows the playlist |
| `TrackList` (queue) | not seen | `HasTrackList = false` |

Consequences: queue/playlist listing needs the Spotify Web API; TIDAL has no browse at all (tidal-hifi's 28 endpoints contain no queue/playlist); TIDAL shuffle/repeat/quality come from the optional tidal-hifi supplement.

Windows GSMTC **(unverified)**: now-playing, cover thumbnail, position, play/pause/next/prev and seek exist; shuffle/repeat exist where the app reports them; volume is not part of GSMTC (per-app audio session API); no queue/browse concept.

## Transport and conventions

- HTTP/1.1 + JSON (UTF-8), base `http://<host>:<port>/v0`. Same style as tidal-hifi so the device's `HttpClient` is enough. Default port: 47900 (the bridge's `--port`; 47836 is tidal-hifi's).
- Commands are `POST`; reads are `GET`. Bodies are small JSON objects.
- Errors: non-2xx with `{"error": "<code>", "message": "<text>"}`. Codes: `no_player`, `unsupported`, `bad_request`, `upstream_failed`, `auth_required`.
- Polling first (`GET /state` every ~500 ms like today). Optional later: `GET /events` as Server-Sent Events carrying the same payload as `/state`, so the device can stop polling. Not needed for v0.
- Auth: LAN only for v0, optional shared token in `Authorization: Bearer <token>` (set in the device's settings page). Pairing and TLS: open question.
- Versioning: the path prefix (`/v0`) plus `GET /info` returning `{"protocol": 0, ...}`. Unknown JSON fields must be ignored by both sides.

## Players

The bridge sees several players at once (e.g. TIDAL and Spotify open together). It exposes them as a list and one **selected** player; `/state` and all commands act on the selected one unless `player` is given.

```
GET /players
{
  "selected": "spotify",
  "players": [
    {"id": "spotify", "name": "Spotify", "service": "spotify", "status": "playing"},
    {"id": "tidal",   "name": "TIDAL",   "service": "tidal",   "status": "paused"}
  ]
}

POST /players/select   {"id": "tidal"}
```

- `id` is chosen by the bridge and stable across restarts of the player (derived from `Identity`/`DesktopEntry`, **not** the bus name).
- `service` is a short key the UI may use for an icon: `spotify`, `tidal`, `browser`, `other`.
- Auto-select rule (default): keep the current selection while it is playing; otherwise pick the most recently playing player. The user's explicit selection wins until that player disappears.

## Now playing

```
GET /state[?player=<id>]
{
  "player": "spotify",
  "status": "playing",                 // playing | paused | stopped
  "track": {
    "id": "spotify:track:2A3ShNautqXezP1lFDePwa",   // stable key; service URI when known
    "title": "Give Love",
    "artists": ["AKMU"],
    "album": "PLAY",
    "duration_ms": 176532,
    "cover": "c1f3a9"                  // opaque key, changes when the artwork changes; null if none
  },
  "position_ms": 11885,
  "position_at_ms": 1760580000123,     // bridge clock when position_ms was read
  "rate": 1.0,
  "volume": 1.0,                       // 0.0..1.0, null if unknown
  "shuffle": false,                    // null if unsupported
  "repeat": "off",                     // off | all | single, null if unsupported
  "quality": {"label": "HI_RES_LOSSLESS", "badge": "24-bit 48.0kHz"},  // null if unknown
  "capabilities": ["seek","volume","shuffle","repeat","prev_next","browse_playlists","browse_queue","open_uri"]
}
```

Notes:
- Position is **milliseconds**, with `position_at_ms` so the UI can interpolate. This replaces tidal-hifi's whole-second `currentInSeconds` (a likely source of lyrics sync error).
- `artists` is an array (the old `MediaInfo.artists` is a joined string; `BridgeSource` joins it).
- `track.id` must change whenever the track changes. Metadata can be briefly inconsistent after a skip (new id, old title, seen on tidal-hifi); the bridge should debounce so `/state` does not mix tracks, and the device should still key derived data on all fields.
- Mapping to `MediaInfo`: `status`, `title`, `album`, `volume`, `shuffle`, `repeat` map directly; `current_sec`/`duration_sec` = ms/1000; `track_id` = `track.id`; `quality` = `quality.label`, `quality_badge` = `quality.badge`; `image_url` = `/cover/<cover>` on the bridge.

### Capabilities

Strings in `capabilities`, one per feature, so the UI hides what is missing (same idea as `MediaCapability` today):

| string | meaning | today's `MEDIA_CAP_*` |
|---|---|---|
| `seek` | absolute seek | `SEEK` |
| `volume` | player volume get/set | `VOLUME` |
| `shuffle` | shuffle state + toggle | `SHUFFLE` |
| `repeat` | repeat state + cycle | `REPEAT` |
| `prev_next` | previous/next | `PREV_NEXT` |
| `quality` | audio quality reported | new |
| `browse_playlists` | list playlists and their tracks | new (`BROWSE`) |
| `browse_queue` | read the play queue | new (`BROWSE`) |
| `open_uri` | start playback of a URI | new |

Capabilities are per player and can change at runtime (e.g. the Spotify Web API backend is not logged in yet), so they ride along in `/state` rather than being fetched once.

## Cover art

The device must never fetch a cover from a host-local path or a service CDN on its own (TIDAL's cover is a temp file on the host; HTTPS costs RAM on the ESP32). The bridge serves covers:

```
GET /cover/<cover-key>?px=400      → image bytes (JPEG preferred, baseline), Content-Type set correctly
```

The bridge resolves the source (file, URL), resizes to about `px`, re-encodes as **baseline JPEG** (sidesteps progressive-JPEG decoding on the ESP), and caches it. This also fixes the stale/mislabelled-cover problems noted for tidal-hifi.

## Transport commands

All `POST`, optional `?player=<id>`; success is `204`, `unsupported` if the capability is missing.

| endpoint | body | notes |
|---|---|---|
| `/cmd/playpause`, `/cmd/play`, `/cmd/pause` | – | |
| `/cmd/next`, `/cmd/previous` | – | |
| `/cmd/seek` | `{"position_ms": 12000}` | absolute |
| `/cmd/volume` | `{"volume": 0.5}` | 0.0–1.0 |
| `/cmd/shuffle` | `{"enabled": true}` | explicit value, not a toggle (the bridge knows the state; avoids double toggles) |
| `/cmd/repeat` | `{"mode": "all"}` | `off` / `all` / `single` |
| `/cmd/open` | `{"uri": "spotify:playlist:..."}` | needs `open_uri`; Spotify via MPRIS `OpenUri` |

The device's `toggle_shuffle()` / `toggle_repeat()` (today's `MediaSource` API) are computed on the device from the last state and sent as explicit values.

## Browse (Spotify first)

Read-only listing; playback goes through `/cmd/open` or `/cmd/play_item`. Needs the Spotify Web API backend (OAuth, held by the bridge, never by the device). Scope: playlists and the current queue; **no search**.

```
GET /browse/playlists
{"items": [{"id":"spotify:playlist:...", "name":"...", "track_count": 42, "cover":"<key>"}], "next": null}

GET /browse/playlists/<id>/tracks?offset=0&limit=50
{"items": [{"index": 0, "id":"spotify:track:...", "title":"...", "artists":["..."], "duration_ms": 123000, "cover":"<key>"}],
 "total": 120, "next": 50}

GET /browse/queue
{"now": {...track...}, "items": [{...track...}], "truncated": true}
// a queue track may also carry "context" (the playlist it plays from) and "index" (its position there), both null
// when unknown; the device only lets the user tap rows that have both.

POST /cmd/play_item   {"context": "spotify:playlist:...", "index": 7}
```

Known limits:
- Spotify has **no "jump to queue item N"**. Queue entries are view-only unless they belong to a known context, in which case `play_item` with `context` + `index` (Web API `offset`) can jump. The UI should show queue rows as non-interactive when no context is known. **(unverified: how the queue endpoint reports context)**
- The Web API queue is length-limited, hence `truncated`.
- Whether `OpenUri` can start at an offset: **no way found** (URIs carry no offset), hence `play_item` goes through the Web API.
- TIDAL: no `browse_*` capabilities.
- Web API terms for new apps (development-mode limits, Premium requirement for control) must be re-checked against the current docs before building. **(unverified)**

## Bridge internals (not part of the wire protocol)

Per player the bridge merges backends:

1. **Base**: the OS session (MPRIS/GSMTC) → status, metadata, position, controls.
2. **Supplement** (optional, per `service`): tidal-hifi → `shuffle`, `repeat`, `quality` for TIDAL; Spotify Web API → browse.
3. Merge rule: base wins for anything it provides; a supplement fills `null` fields and adds capabilities. A supplement that fails or is not configured only removes its capabilities.

Player identification: Linux `Identity`/`DesktopEntry`; Windows the GSMTC app user model id **(unverified)**.

## Open questions

1. Bridge implementation language (Rust suggested: single binary, `zbus` for MPRIS, `windows` crate for GSMTC). Not decided.
2. Default port, discovery (mDNS vs typed address; see the settings page), token/pairing, whether TLS is ever needed on a LAN.
3. SSE vs polling once the bridge exists.
4. How lyrics fit: the bridge could host the lookup later (deferred; also carries the ISRC idea).
5. `quality` for non-TIDAL players: probably always `null`.
6. Player auto-select rule and a device-side source-switch gesture (long-press/touch is reserved for this).
7. What happens to `TidalClient`: kept as the TIDAL supplement for now; may move into the bridge.
8. Windows: confirm GSMTC capability coverage per player (shuffle/repeat/seek) on a real Windows machine.
