# Hub protocol (draft v1): the device is the hub

Status: **draft, first version implemented** (2026-10-17): the simulator is the hub (`lvglsim -S <port>`, `src/hub_source.*` on `src/platform/ws_server.h`, libwebsockets in `platform_sim`) and the bridge is a source (`bridge/src/hubclient.rs`). Working: `hello`/`auth`/`pair`, `players` and `state` push, `focus`, `cmd`/`ack`, `cover_req` with the binary reply. mDNS discovery works too (simulator: `platform_sim/mdns_responder.cpp`; bridge: `discovery.rs` with `mdns-sd`). `lyrics_req`/`lyrics` work too: the bridge announces the `lyrics` feature and does the LRCLIB lookup (`bridge/src/lyrics.rs`, a port of `src/lyrics.cpp` with a disk cache); a source without the feature makes the device fetch from LRCLIB itself. **Not yet:** the ESP32 server (`esp_http_server`), TLS. Written 2026-10-17. It replaces the direction of
`BRIDGE_PROTOCOL.md` (v0: the device polls a server on the PC, implemented in `bridge/` and `BridgeSource`) while keeping
its vocabulary: players, `state`, capabilities, commands. v0 stays working until v1 can take over. Everything about
ESP32 load is an estimate, not a measurement (no hardware yet).

## Why turn it around

The device (ESP32-S3, or the simulator) is what the user holds; the PCs are interchangeable *sources* of "what is playing".
So the device is the server and the center:

- it **owns the connection point**: sources find it (mDNS) and connect to it, instead of the device being given each PC's
  IP address; a PC whose IP changes just reconnects;
- it **owns the decisions**: which source/player is shown, when to switch, which sources are trusted;
- several sources at once (a Windows PC and a Linux PC) need no per-source setup on the device, only a pairing;
- sources connect *outwards*, so a PC firewall needs no rule for incoming connections;
- state is **pushed on change** instead of polled every 500 ms, which is also less work for the ESP32.

```
 source (PC bridge, Linux)  ──┐                          ┌─ MPRIS players
 source (PC bridge, Windows) ─┼─ ws://device:47900/source ─▶ DEVICE (hub): shows the focused player,
 source (...)               ──┘      (sources connect)     └─ sends commands back over the same socket
```

Terms: **device** = the hub (ESP32 / simulator). **source** = a program that connects to the device and represents a set of
**players** (the PC bridge: Spotify, TIDAL, a browser, ...). A player is identified by `(source_id, player_id)`.

## Transport

- WebSocket (RFC 6455), plain `ws://` (TLS is an open question, see the end), on TCP port **47900** of the device, path
  `/source`. The device runs the server (ESP-IDF `esp_http_server` with WebSocket support, or a small library in the
  simulator); the source is the client and reconnects with back-off (1 s doubling to 30 s).
- **Text frames** carry one JSON object each, `{"t": "<type>", ...}`. Unknown types and fields are ignored.
  **Binary frames** carry covers: `[u32 big-endian request id][JPEG bytes]`.
- Limits (the device announces its own in `welcome`): text frames ≤ 16 KB (lyrics ≤ 64 KB), binary frames ≤ 256 KB,
  at most **4 sources** at the same time (a fifth connection is refused with `busy`).
- WebSocket ping/pong every 10 s from the device; a source silent for 25 s is dropped and its players disappear.
- Messages carrying a request id (`id`, an integer chosen by the sender) are answered with the same `id`.

## Discovery

The device advertises itself over mDNS/DNS-SD: service `_musicbridge._tcp`, its WebSocket port, host name `music-controller.local`
(editable on the device), TXT records `proto=1`, `name=<device name>`, `id=<device id>`. Implemented behaviour: it announces twice on start, answers PTR/SRV/TXT/A questions
(on the interface the question came in on, with that interface's address as the A record) and sends a goodbye (TTL 0) when it exits cleanly; a source that
is killed lingers in others' caches until the 120 s TTL runs out. No probing or conflict handling. A source browses for it. A device it already knows (by `id`) is followed to its new address automatically; new ones are listed (the bridge's settings page, "Found nearby") for the user to connect to, then pair. A source may also
be given an address by hand (`host[:port]`) when mDNS does not cross a network.

## Pairing and trust

No long token to type on a touch screen. The device shows a short code, the PC's keyboard types it:

1. The source connects and sends `hello` (below). The device answers `welcome` and, if it does not know this `source_id`,
   `{"t":"pairing_required"}`.
2. The user opens **Sources → Pair a source** on the device. It shows a 6-digit code valid for 2 minutes and accepts
   pairing from any connected source meanwhile.
3. The user types the code into the source (the bridge's settings page). The source sends `{"t":"pair","code":"123456"}`.
   The device checks it (3 wrong tries lock pairing mode), creates a random 128-bit **secret**, stores
   `(source_id, name, secret)` and answers `{"t":"paired","secret":"<hex>","device_id":"...","name":"..."}`. The source stores it.
4. Later connections skip the code: `hello`, then `{"t":"auth","secret":"<hex>"}`; the device answers `{"t":"ready"}`
   or `{"t":"auth_failed"}` (secret revoked, or the device was reset: the source goes back to asking for a code).
5. The device's **Sources** list shows paired sources (name, connected or not) and lets the user remove one (its secret dies).

Until `ready`/`paired`, the device accepts nothing but `hello`, `auth` and `pair`. Everything is in the clear on the LAN: the
secret and the code can be sniffed by someone on the same network (TLS/PAKE is an open question).

## Messages: source → device

| `t` | fields | meaning |
|---|---|---|
| `hello` | `proto` (1), `source_id` (stable UUID), `name` ("Arch PC"), `os` (`linux`/`windows`), `version`, `features` (`["lyrics","cover"]`) | first message |
| `auth` / `pair` | `secret` / `code` | see Pairing; a wrong or late `pair` is answered with `{"t":"pair_failed","reason":"wrong_code"|"closed"}` |
| `players` | `players`: `[{"id","name","service","status"}]`, `selected` (the source's own pick: an id, or `null`) | the complete list, sent when it changes (a player appears, disappears, starts/stops, or the pick changes) |
| `state` | one player's state, see below | sent when anything but the position changes, and when the position jumps (seek, track change) |
| `ack` | `id`, `ok` (bool), `error` (`unsupported`, `no_player`, `upstream_failed`, `bad_request`) | answer to a `cmd` |
| `lyrics` | `id`, `found`, `synced`, `lines`: `[{"ms","text"}]`, optional `error` | answer to `lyrics_req`. `ms` is 0 for unsynced lyrics; a line with an empty `text` is an instrumental gap (the end marker after the last line is already included); the list is cut to fit 64 KB; `found: false` + `error: "upstream_failed"` means the source could not find out (not remembered as "no lyrics") |
| `cover_err` | `id` | no cover for that request (a successful one is a binary frame) |

### `state` (per player)

Same content as v0's `/state`, with one change: no `position_at_ms` (device and source clocks are not compared); the position
is valid *when the message arrives*.

```json
{ "t": "state", "player": "spotify", "status": "playing",
  "track": { "id": "spotify:track:2A3Sh...", "title": "Give Love", "artists": ["AKMU"], "album": "PLAY",
             "duration_ms": 176532, "cover": "c1f3a9" },
  "position_ms": 11885, "rate": 1.0, "volume": 1.0, "shuffle": false, "repeat": "off",
  "quality": null,
  "capabilities": ["seek","volume","shuffle","repeat","prev_next","open_uri"] }
```

- The device **interpolates**: while `status` is `playing` the shown position is `position_ms + rate × (now − arrival)`.
  The source corrects it by sending a new `state` when the real position drifts more than 1 s from that line (seeks included).
  Extra `state` messages are cheap; a source should also repeat one every 30 s as a heartbeat of the position.
- `track.cover` is an opaque key that changes with the artwork. `quality`, `shuffle`, `repeat` are `null` when unknown;
  capabilities decide which controls the device shows (as in v0). Capability strings: `seek`, `volume`, `shuffle`, `repeat`,
  `prev_next`, `open_uri`, `quality`, and (roadmap, nothing sends them yet) `browse_playlists`, `browse_queue`.
- A `state` for an unknown player id, or one not in the last `players`, is ignored.

## Messages: device → source

| `t` | fields | meaning |
|---|---|---|
| `welcome` | `proto`, `device_id`, `name`, `max_text`, `max_binary` | answer to `hello` |
| `pairing_required`, `paired`, `ready`, `auth_failed` | see Pairing | |
| `busy` | | too many sources; the socket is closed |
| `focus` | `player` (id) or `null` | which player of this source the user is looking at. Lets the source work on that one first (fast polling, prefetch the cover and lyrics) |
| `cmd` | `id`, `player`, `op`, and arguments | a transport command |
| `cover_req` | `id`, `key`, `px` | wants the cover `key` at about `px` pixels; answered by a binary frame `[id][JPEG]` or `cover_err` |
| `lyrics_req` | `id`, `player`, `track_id`, `title`, `artists`, `album`, `duration_ms` | wants lyrics for that track (only sent to sources announcing the `lyrics` feature) |

`cmd` ops (all answered by `ack`): `playpause`, `play`, `pause`, `next`, `previous`, `seek` (`position_ms`), `volume`
(`value` 0..1), `shuffle` (`enabled`), `repeat` (`mode`: `off`/`all`/`single`), `open` (`uri`). Semantics are those of v0's
`/cmd/*`. The device coalesces volume steps to one `cmd` per 80 ms, like today.

## What the source does

- Reads its players (the bridge: MPRIS on Linux, GSMTC on Windows, plus the tidal-hifi supplement) and keeps a local view
  by polling the OS every ~500 ms (the OS offers no push worth the trouble on all platforms). Only changes go to the device.
- **Covers**: re-encodes to baseline JPEG at the requested size (as the v0 bridge does), so the ESP32 never meets a
  progressive JPEG, PNG or WebP. One reply per request; the device caches by `key`.
- **Lyrics**: the lookup (LRCLIB, ranking, LRC parsing, TLS) moves to the source, which has the CPU, the TLS stack and a
  place to cache. The bridge runs one lookup at a time (LRCLIB answers 429 to bursts), drops a lookup when a newer `lyrics_req` arrives, keeps found lyrics in
  `~/.cache/music-bridge/lyrics/` and remembers "nothing found" for a week (a failed lookup is not remembered). The ESP32 then needs no HTTPS at all. A source without the `lyrics` feature gets no `lyrics_req` and the
  device shows no lyrics for its players.
- Keeps its pairing secrets and its `source_id` in its own config; may be paired with several devices and connects to each.

## What the device does

- Keeps every connected source's players and last `state`. **Focus** = the player shown: pinned by the user on the device, or
  *automatic*: every source proposes its `selected` player (the one picked on the PC's settings page, else what plays there; a source
  that sends none proposes any playing player), and of the proposals that are playing the one that started last wins; while none is
  playing the device shows the proposal of the source it looked at last. So a pick made on the PC holds even when a browser video
  starts, and a device can still override everything by pinning a player.
- Settings screens: **Sources** (list, pair, remove), **Player** (every player grouped by source, "Automatic" first; replaces
  today's picker), WiFi, device name.
- Sends `focus` whenever it changes, `cmd` for buttons/encoder/seek, `cover_req` when it lacks a cover, `lyrics_req` for the
  focused track.
- Treats a missing source like today's "connection lost": the banner shows when no source is connected or the focused
  player vanished.

## ESP32 budget (estimates)

| Item | Estimate |
|---|---|
| WebSocket server task + 4 sockets | a few KB of internal RAM per socket (lwIP buffers); keep `CONFIG_LWIP_MAX_SOCKETS` ≥ 8 |
| One `state` message | ~1 KB, parsed with nlohmann_json in about a millisecond; a few per minute while idle |
| mDNS | a few KB |
| Cover | one ≤256 KB binary frame into PSRAM, then the existing decode path (baseline JPEG, 400 px) |
| Lyrics | ≤64 KB text frame, parsed once per track (the JSON array replaces the LRC parser on the device) |
| TLS | none (it was the heaviest item of the v0 design) |

Not covered by the estimates, to be measured first on hardware: WiFi + RGB panel + PSRAM contention, JPEG decode time on the
S3 (no hardware decoder), real internal-RAM headroom with the WebSocket server running.

## Migration and what changes in this repo

- `bridge/`: gains a **hub client** (find devices over mDNS, pair, connect, push `state`, answer `cmd`/`cover_req`/`lyrics_req`);
  the settings page gets a *Devices* section (found devices, enter a pairing code, connected or not) and the v0 token
  disappears from the normal path. The v0 HTTP API can stay behind a flag for development.
- `src/`: a `HubServer` (platform interface: `platform_sim` uses a small WebSocket server library, ESP-IDF uses
  `esp_http_server`) feeding a **source registry** that implements what `MediaSource` does today for one source, but for
  many; `BridgeSource` and the token setting become the legacy/dev path. Lyrics fetching in `lyrics.*` stays for the
  direct tidal-hifi mode.
- The device-side player choice (`AppSettings::player`, the Player page) is kept; its id becomes `(source_id, player_id)`.

## Open questions

1. TLS: `wss://` needs a certificate the source trusts (self-signed, pinned at pairing?) and costs ESP32 RAM; a PAKE for the
   pairing code would protect the secret on plain ws. Both can come after the first version.
2. mDNS across subnets/VLANs and on Windows (the bridge resolves it itself, no OS service needed), and a manual-address fallback.
3. Library for the simulator's WebSocket server (libwebsockets, civetweb, or a small hand-written one over the existing
   sockets); hidden behind `platform/`.
4. A source that is *also* the lyrics provider: caching policy, and what the device shows while lyrics are loading.
5. Several devices and one source: allowed (the source connects to each), but volume/shuffle state then belongs to the
   player, not the device; the `state` stream keeps the devices consistent.
6. The default port 47900 (like tidal-hifi's 47836) lies inside Linux's ephemeral port range (32768-60999): once in a while an outgoing connection of some program holds it and `bind` fails with EADDRINUSE (seen once in testing). A port below 32768 would avoid it; not changed yet.
7. Quick source/player switching on the main screen (gesture or button): still undecided, see AGENTS.md "Encoder UX".
