//! Player registry, selection and the mapping from the backend's raw player data (player.rs) to the protocol's types.

use std::collections::{HashMap, HashSet};
use std::sync::{Arc, Mutex, RwLock};
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

use axum::Json;
use axum::http::StatusCode;
use axum::response::{IntoResponse, Response};

use crate::cover;
use crate::model::*;
use crate::player::{Backend, Command, PlayerBackend, Raw};
use crate::tidal::Tidal;

pub struct ApiError {
    status: StatusCode,
    error: &'static str,
    message: String,
}

impl ApiError {
    /// The protocol's error code (`unsupported`, `no_player`, ...).
    pub fn code(&self) -> &'static str {
        self.error
    }

    fn new(status: StatusCode, error: &'static str, message: impl Into<String>) -> Self {
        Self {
            status,
            error,
            message: message.into(),
        }
    }
}

impl IntoResponse for ApiError {
    fn into_response(self) -> Response {
        (
            self.status,
            Json(ErrBody {
                error: self.error,
                message: self.message,
            }),
        )
            .into_response()
    }
}

/// A command from a device (or a `/v0` client), independent of how the player is driven: `command()` maps it onto MPRIS
/// or, for shuffle and repeat, onto the tidal-hifi supplement.
pub enum Cmd {
    PlayPause,
    Play,
    Pause,
    Next,
    Previous,
    SeekMs(i64),
    Volume(f64),
    Shuffle(bool),
    Repeat(String),
    Open(String),
}

/// What the hub remembers between two readings of the system.
#[derive(Default)]
struct Inner {
    /// the player a device or the settings page chose; it wins over `auto` for as long as that player exists
    explicit: Option<String>,
    /// the automatic pick, kept while that player still plays so that it does not jump between two playing players
    auto: Option<String>,
    /// since when each player has been playing, to find the newest
    playing_since: HashMap<String, Instant>,
    /// cover key -> the player's artwork URL: the protocol only carries the short key, the URL is looked up when the
    /// cover is asked for
    cover_urls: HashMap<String, String>,
    /// covers already downloaded and re-encoded, by (key, size)
    cover_cache: HashMap<(String, u32), Arc<Vec<u8>>>,
    /// per player: the cover key last reported, and since when tidal-hifi has been on another track than MPRIS (see state_of)
    cover_shown: HashMap<String, (Option<String>, Option<Instant>)>,
}

/// How long the cover of the last track stays while tidal-hifi still talks about the previous one.
const COVER_HOLD: Duration = Duration::from_secs(3);

pub struct Hub {
    backend: Backend,
    inner: Mutex<Inner>,
    tidal: RwLock<Option<Arc<Tidal>>>,
    /// players the user switched off for devices: they take no part in the selection, the list, the states or the commands
    disabled: RwLock<HashSet<String>>,
}

struct Snapshot {
    players: Vec<(String, Raw)>,
    /// running players that are switched off (only the settings page lists them)
    hidden: Vec<(String, Raw)>,
    selected: Option<String>,
}

/// A player id from a name: lower case, every run of characters that are not letters or digits becomes one `-`. Ids come
/// from the desktop entry or the identity, never from the bus name, which can change (tidal-hifi's contains its PID).
fn slug(s: &str) -> String {
    let mut out: String = s
        .to_lowercase()
        .chars()
        .map(|c| if c.is_ascii_alphanumeric() { c } else { '-' })
        .collect();
    while out.contains("--") {
        out = out.replace("--", "-");
    }
    out.trim_matches('-').to_string()
}

/// Which service a player is, guessed from its identity, desktop entry and backend id.
fn service_of(raw: &Raw) -> &'static str {
    let hay = format!(
        "{} {} {}",
        raw.identity,
        raw.desktop_entry.clone().unwrap_or_default(),
        raw.app_id
    )
    .to_lowercase();
    if hay.contains("kdeconnect") {
        "other" // a phone mirrored through KDE Connect: its app names say nothing about this PC's players
    } else if hay.contains("spotify") {
        "spotify"
    } else if hay.contains("tidal") {
        "tidal"
    } else if ["chrom", "firefox", "brave", "edge", "vivaldi", "opera"]
        .iter()
        .any(|b| hay.contains(b))
    {
        "browser"
    } else {
        "other"
    }
}

fn status_of(raw: &Raw) -> &'static str {
    match raw.status.as_str() {
        "Playing" => "playing",
        "Paused" => "paused",
        _ => "stopped",
    }
}

/// A small non-cryptographic hash (FNV-1a) that turns a cover URL into a short key. Nothing secret depends on it.
fn fnv1a(s: &str) -> u64 {
    s.bytes().fold(0xcbf29ce484222325, |h, b| {
        (h ^ b as u64).wrapping_mul(0x100000001b3)
    })
}

fn now_ms() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|d| d.as_millis() as u64)
        .unwrap_or(0)
}

/// A stable id for the track. Spotify's web URL becomes `spotify:track:<id>`; otherwise MPRIS's own track id; and when the
/// player has none (`NoTrack`, `/`) the title, artists and album joined, which is at least the same while it plays.
fn track_id(raw: &Raw) -> String {
    let m = &raw.meta;
    if let Some(u) = &m.xesam_url
        && let Some(id) = u.split("open.spotify.com/track/").nth(1)
    {
        return format!(
            "spotify:track:{}",
            id.split(['?', '/']).next().unwrap_or(id)
        );
    }
    match &m.trackid {
        Some(t) if t != "/org/mpris/MediaPlayer2/TrackList/NoTrack" && t != "/" => t.clone(),
        _ => format!("{}|{}|{}", m.title, m.artists.join(", "), m.album),
    }
}

/// What the player can do, from the properties it exposes. A control the player does not have is simply missing from the
/// list, which is what hides it on the device.
fn capabilities(raw: &Raw) -> Vec<&'static str> {
    let mut c = Vec::new();
    if raw.can_seek {
        c.push("seek");
    }
    if raw.volume.is_some() {
        c.push("volume");
    }
    if raw.shuffle.is_some() {
        c.push("shuffle");
    }
    if raw.loop_status.is_some() {
        c.push("repeat");
    }
    if raw.can_next || raw.can_prev {
        c.push("prev_next");
    }
    if !raw.uri_schemes.is_empty() {
        c.push("open_uri");
    }
    c
}

impl Hub {
    pub async fn new(tidal: Option<Tidal>) -> Result<Arc<Self>, String> {
        Ok(Arc::new(Self {
            backend: Backend::connect().await?,
            inner: Mutex::new(Inner::default()),
            tidal: RwLock::new(tidal.map(Arc::new)),
            disabled: RwLock::new(HashSet::new()),
        }))
    }

    /// The tidal-hifi supplement, for the player it belongs to.
    fn tidal_for(&self, raw: &Raw) -> Option<Arc<Tidal>> {
        if raw.identity.eq_ignore_ascii_case("tidal-hifi") {
            self.tidal.read().unwrap().clone()
        } else {
            None
        }
    }

    /// Switch the tidal-hifi supplement on (Some) or off (None) at runtime.
    pub fn set_tidal(&self, t: Option<Tidal>) {
        *self.tidal.write().unwrap() = t.map(Arc::new);
    }

    /// Replace the set of players that are not sent to devices.
    pub fn set_disabled(&self, ids: impl IntoIterator<Item = String>) {
        *self.disabled.write().unwrap() = ids.into_iter().collect();
    }

    pub fn disabled(&self) -> HashSet<String> {
        self.disabled.read().unwrap().clone()
    }

    /// (address, answers right now) of the tidal-hifi supplement, None when it is off.
    pub async fn tidal_status(&self) -> Option<(String, bool)> {
        let t = self.tidal.read().unwrap().clone()?;
        Some((t.spec().to_string(), t.current().await.is_some()))
    }

    /// Read every player and settle the selection. A second player with the same base id gets a `-2` suffix. The choice
    /// made explicitly stays while that player exists. Otherwise the automatic pick stays on its player as long as it
    /// plays, moves to the newest playing player when it does not, and with nothing playing falls back to the first one.
    async fn snapshot(&self) -> Snapshot {
        let mut players: Vec<(String, Raw)> = Vec::new();
        let mut taken: HashMap<String, u32> = HashMap::new();
        for raw in self.backend.players().await {
            let base = slug(
                raw.desktop_entry
                    .as_deref()
                    .filter(|s| !s.is_empty())
                    .unwrap_or(&raw.identity),
            );
            let n = taken.entry(base.clone()).or_insert(0);
            *n += 1;
            let id = if *n == 1 { base } else { format!("{base}-{n}") };
            players.push((id, raw));
        }
        // switched-off players are set aside before the selection is settled, so neither the automatic pick nor a device can land on one
        let disabled = self.disabled.read().unwrap().clone();
        let (hidden, players): (Vec<_>, Vec<_>) = players
            .into_iter()
            .partition(|(id, _)| disabled.contains(id));

        let mut inner = self.inner.lock().unwrap();
        let now = Instant::now();
        for (id, raw) in &players {
            if raw.status == "Playing" {
                inner.playing_since.entry(id.clone()).or_insert(now);
            } else {
                inner.playing_since.remove(id);
            }
        }
        let ids: Vec<&String> = players.iter().map(|(id, _)| id).collect();
        inner.playing_since.retain(|id, _| ids.contains(&id));

        let exists = |id: &Option<String>| id.as_ref().is_some_and(|i| ids.contains(&i));
        if !exists(&inner.explicit) {
            inner.explicit = None;
        }
        if !exists(&inner.auto)
            || !inner
                .playing_since
                .contains_key(inner.auto.as_ref().unwrap())
        {
            let newest = inner
                .playing_since
                .iter()
                .max_by_key(|(_, t)| **t)
                .map(|(id, _)| id.clone());
            if let Some(id) = newest {
                inner.auto = Some(id);
            } else if !exists(&inner.auto) {
                inner.auto = ids.first().map(|s| (*s).clone());
            }
        }
        let selected = inner.explicit.clone().or_else(|| inner.auto.clone());
        Snapshot {
            players,
            hidden,
            selected,
        }
    }

    /// The player a request means: the named one, or the selected one when none is named. `no_player` if it is not there.
    fn pick<'a>(snap: &'a Snapshot, player: Option<&str>) -> Result<&'a (String, Raw), ApiError> {
        let want = player.map(str::to_string).or_else(|| snap.selected.clone());
        want.as_ref()
            .and_then(|w| snap.players.iter().find(|(id, _)| id == w))
            .ok_or_else(|| ApiError::new(StatusCode::NOT_FOUND, "no_player", "no such player"))
    }

    pub async fn info(&self) -> Info {
        Info {
            protocol: 0,
            name: "raira-bridge",
            version: env!("CARGO_PKG_VERSION"),
        }
    }

    pub async fn players(&self) -> Players {
        let snap = self.snapshot().await;
        Players {
            selected: snap.selected.clone(),
            players: snap
                .players
                .iter()
                .map(|(id, raw)| PlayerEntry {
                    id: id.clone(),
                    name: raw.identity.clone(),
                    service: service_of(raw),
                    status: status_of(raw),
                })
                .collect(),
        }
    }

    /// What the settings page lists: the players devices see (with the selection) and, apart, the running ones that are switched off.
    pub async fn players_with_hidden(&self) -> (Players, Vec<PlayerEntry>) {
        let snap = self.snapshot().await;
        let entry = |(id, raw): &(String, Raw)| PlayerEntry {
            id: id.clone(),
            name: raw.identity.clone(),
            service: service_of(raw),
            status: status_of(raw),
        };
        (
            Players {
                selected: snap.selected.clone(),
                players: snap.players.iter().map(entry).collect(),
            },
            snap.hidden.iter().map(entry).collect(),
        )
    }

    pub async fn select(&self, id: &str) -> Result<(), ApiError> {
        let snap = self.snapshot().await;
        Self::pick(&snap, Some(id))?;
        self.inner.lock().unwrap().explicit = Some(id.to_string());
        Ok(())
    }

    pub async fn state(&self, player: Option<&str>) -> Result<State, ApiError> {
        let snap = self.snapshot().await;
        let (id, raw) = Self::pick(&snap, player)?;
        Ok(self.state_of(id, raw).await)
    }

    /// The player list and every player's state from one reading of the system (what the hub client pushes).
    pub async fn all_states(&self) -> (Players, Vec<State>) {
        let snap = self.snapshot().await;
        let players = Players {
            selected: snap.selected.clone(),
            players: snap
                .players
                .iter()
                .map(|(id, raw)| PlayerEntry {
                    id: id.clone(),
                    name: raw.identity.clone(),
                    service: service_of(raw),
                    status: status_of(raw),
                })
                .collect(),
        };
        let mut states = Vec::new();
        for (id, raw) in &snap.players {
            states.push(self.state_of(id, raw).await);
        }
        (players, states)
    }

    /// One player's state in the protocol's form. The cover key hashes the artwork URL together with the track id, so it
    /// changes whenever the artwork or the track does. `position_at_ms` says when the position
    /// was read, so that the receiver can move it forward.
    async fn state_of(&self, id: &str, raw: &Raw) -> State {
        let tid = track_id(raw);

        // Supplement: fills what the MPRIS player lacks. It never overrides MPRIS, and a supplement that does not
        // answer only takes its own capabilities away.
        let sup = match self.tidal_for(raw) {
            Some(t) => t.current().await,
            None => None,
        };
        // The cover: MPRIS's art of tidal-hifi is a 150 px file, so for that player the picture TIDAL serves is used, as long
        // as tidal-hifi is on the same track. It lags behind MPRIS for a moment after a skip: the cover that was shown stays
        // for COVER_HOLD then (a blurry stand-in followed by the real one made two transitions, each a full-screen fade);
        // when tidal-hifi does not catch up, or is not there, the MPRIS art is the cover.
        let lagging = sup
            .as_ref()
            .is_some_and(|s| s.title.trim() != raw.meta.title.trim());
        let art_url = sup
            .as_ref()
            .filter(|s| s.title.trim() == raw.meta.title.trim())
            .and_then(|s| s.image.clone())
            .or_else(|| raw.meta.art_url.clone());
        let cover = {
            let mut inner = self.inner.lock().unwrap();
            let entry = inner.cover_shown.entry(id.to_string()).or_default();
            let since = if lagging {
                *entry.1.get_or_insert_with(Instant::now)
            } else {
                entry.1 = None;
                Instant::now()
            };
            let held = if lagging && since.elapsed() < COVER_HOLD {
                entry.0.clone()
            } else {
                None
            };
            let key = held.or_else(|| {
                art_url.as_ref().map(|url| {
                    let key = format!("{:016x}", fnv1a(&format!("{url}|{tid}")));
                    inner.cover_urls.insert(key.clone(), url.clone());
                    key
                })
            });
            inner.cover_shown.entry(id.to_string()).or_default().0 = key.clone();
            key
        };
        let mut caps = capabilities(raw);
        let mut shuffle = raw.shuffle;
        let mut repeat = raw.loop_status.as_deref().map(|l| match l {
            "Playlist" => "all",
            "Track" => "single",
            _ => "off",
        });
        let mut quality = None;
        if let Some(s) = sup {
            if raw.shuffle.is_none() && s.shuffle.is_some() {
                shuffle = s.shuffle;
                caps.push("shuffle");
            }
            if raw.loop_status.is_none() && s.repeat.is_some() {
                repeat = s.repeat;
                caps.push("repeat");
            }
            // tidal-hifi can lag behind MPRIS right after a skip: the quality belongs to *its* track
            if s.title.trim() == raw.meta.title.trim() {
                quality = s.quality.map(|(label, badge)| Quality { label, badge });
                if quality.is_some() {
                    caps.push("quality");
                }
            }
        }

        State {
            player: id.to_string(),
            status: status_of(raw),
            track: Track {
                id: tid,
                title: raw.meta.title.clone(),
                artists: raw.meta.artists.clone(),
                album: raw.meta.album.clone(),
                duration_ms: raw.meta.length_us.map(|us| us / 1000),
                cover,
            },
            position_ms: raw.position_us.map(|us| us / 1000),
            position_at_ms: now_ms(),
            rate: raw.rate,
            volume: raw.volume,
            shuffle,
            repeat,
            quality,
            capabilities: caps,
        }
    }

    /// Runs a command on the named (or selected) player. A control the player does not offer is refused with `unsupported`
    /// instead of being sent; shuffle and repeat that MPRIS lacks go to tidal-hifi when this player is tidal-hifi.
    pub async fn command(&self, player: Option<&str>, cmd: Cmd) -> Result<(), ApiError> {
        let snap = self.snapshot().await;
        let (_, raw) = Self::pick(&snap, player)?;
        let unsupported = |what: &str| {
            ApiError::new(
                StatusCode::BAD_REQUEST,
                "unsupported",
                format!("{what} is not supported by this player"),
            )
        };
        let c = match cmd {
            Cmd::PlayPause => Command::PlayPause,
            Cmd::Play => Command::Play,
            Cmd::Pause => Command::Pause,
            Cmd::Next if raw.can_next => Command::Next,
            Cmd::Previous if raw.can_prev => Command::Previous,
            Cmd::Next | Cmd::Previous => return Err(unsupported("prev/next")),
            Cmd::SeekMs(ms) if raw.can_seek => Command::SeekAbsoluteUs(ms.max(0) * 1000),
            Cmd::SeekMs(_) => return Err(unsupported("seek")),
            Cmd::Volume(v) if raw.volume.is_some() => Command::Volume(v.clamp(0.0, 1.0)),
            Cmd::Volume(_) => return Err(unsupported("volume")),
            Cmd::Shuffle(on) if raw.shuffle.is_some() => Command::Shuffle(on),
            Cmd::Shuffle(on) => {
                let Some(t) = self.tidal_for(raw) else {
                    return Err(unsupported("shuffle"));
                };
                return t
                    .set_shuffle(on)
                    .await
                    .map_err(|e| ApiError::new(StatusCode::BAD_GATEWAY, "upstream_failed", e));
            }
            Cmd::Repeat(mode) if raw.loop_status.is_some() => Command::Loop(match mode.as_str() {
                "off" => "None",
                "all" => "Playlist",
                "single" => "Track",
                _ => {
                    return Err(ApiError::new(
                        StatusCode::BAD_REQUEST,
                        "bad_request",
                        "mode must be off, all or single",
                    ));
                }
            }),
            Cmd::Repeat(mode) => {
                if !matches!(mode.as_str(), "off" | "all" | "single") {
                    return Err(ApiError::new(
                        StatusCode::BAD_REQUEST,
                        "bad_request",
                        "mode must be off, all or single",
                    ));
                }
                let Some(t) = self.tidal_for(raw) else {
                    return Err(unsupported("repeat"));
                };
                return t
                    .set_repeat(&mode)
                    .await
                    .map_err(|e| ApiError::new(StatusCode::BAD_GATEWAY, "upstream_failed", e));
            }
            Cmd::Open(uri) if !raw.uri_schemes.is_empty() => Command::OpenUri(uri),
            Cmd::Open(_) => return Err(unsupported("open_uri")),
        };
        self.backend
            .send(raw, c)
            .await
            .map_err(|e| ApiError::new(StatusCode::BAD_GATEWAY, "upstream_failed", e))
    }

    /// The cover for a key as a JPEG of the wanted size, downloaded and re-encoded on first use and then kept (at most 64
    /// are cached, then the cache starts over).
    pub async fn cover(&self, key: &str, px: u32) -> Result<Arc<Vec<u8>>, ApiError> {
        let (url, cached) = {
            let inner = self.inner.lock().unwrap();
            (
                inner.cover_urls.get(key).cloned(),
                inner.cover_cache.get(&(key.to_string(), px)).cloned(),
            )
        };
        if let Some(c) = cached {
            return Ok(c);
        }
        let url = url.ok_or_else(|| {
            ApiError::new(StatusCode::NOT_FOUND, "bad_request", "unknown cover key")
        })?;
        let upstream = |e: String| ApiError::new(StatusCode::BAD_GATEWAY, "upstream_failed", e);
        let bytes = cover::load(&url).await.map_err(upstream)?;
        let jpeg = Arc::new(cover::to_jpeg(bytes, px).await.map_err(upstream)?);

        let mut inner = self.inner.lock().unwrap();
        if inner.cover_cache.len() >= 64 {
            inner.cover_cache.clear(); // small and simple: covers are cheap to rebuild
        }
        inner
            .cover_cache
            .insert((key.to_string(), px), jpeg.clone());
        Ok(jpeg)
    }
}
