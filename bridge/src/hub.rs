//! Player registry, selection and the mapping from raw MPRIS data to the protocol's types.

use std::collections::HashMap;
use std::sync::{Arc, Mutex};
use std::time::{Instant, SystemTime, UNIX_EPOCH};

use axum::Json;
use axum::http::StatusCode;
use axum::response::{IntoResponse, Response};

use crate::cover;
use crate::model::*;
use crate::mpris::{Command, Mpris, Raw};
use crate::tidal::Tidal;

pub struct ApiError {
    status: StatusCode,
    error: &'static str,
    message: String,
}

impl ApiError {
    fn new(status: StatusCode, error: &'static str, message: impl Into<String>) -> Self {
        Self { status, error, message: message.into() }
    }
}

impl IntoResponse for ApiError {
    fn into_response(self) -> Response {
        (self.status, Json(ErrBody { error: self.error, message: self.message })).into_response()
    }
}

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

#[derive(Default)]
struct Inner {
    explicit: Option<String>,
    auto: Option<String>,
    playing_since: HashMap<String, Instant>,
    cover_urls: HashMap<String, String>,
    cover_cache: HashMap<(String, u32), Arc<Vec<u8>>>,
}

pub struct Hub {
    mpris: Mpris,
    inner: Mutex<Inner>,
    tidal: Option<Tidal>,
}

struct Snapshot {
    players: Vec<(String, Raw)>,
    selected: Option<String>,
}

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

fn service_of(raw: &Raw) -> &'static str {
    let hay = format!("{} {} {}", raw.identity, raw.desktop_entry.clone().unwrap_or_default(), raw.bus).to_lowercase();
    if hay.contains("kdeconnect") {
        "other" // a phone mirrored through KDE Connect: its app names say nothing about this PC's players
    } else if hay.contains("spotify") {
        "spotify"
    } else if hay.contains("tidal") {
        "tidal"
    } else if ["chrom", "firefox", "brave", "edge", "vivaldi", "opera"].iter().any(|b| hay.contains(b)) {
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

fn fnv1a(s: &str) -> u64 {
    s.bytes().fold(0xcbf29ce484222325, |h, b| (h ^ b as u64).wrapping_mul(0x100000001b3))
}

fn now_ms() -> u64 {
    SystemTime::now().duration_since(UNIX_EPOCH).map(|d| d.as_millis() as u64).unwrap_or(0)
}

fn track_id(raw: &Raw) -> String {
    let m = &raw.meta;
    if let Some(u) = &m.xesam_url {
        if let Some(id) = u.split("open.spotify.com/track/").nth(1) {
            return format!("spotify:track:{}", id.split(['?', '/']).next().unwrap_or(id));
        }
    }
    match &m.trackid {
        Some(t) if t != "/org/mpris/MediaPlayer2/TrackList/NoTrack" && t != "/" => t.clone(),
        _ => format!("{}|{}|{}", m.title, m.artists.join(", "), m.album),
    }
}

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
    pub async fn new(tidal: Option<Tidal>) -> zbus::Result<Arc<Self>> {
        Ok(Arc::new(Self { mpris: Mpris::connect().await?, inner: Mutex::new(Inner::default()), tidal }))
    }

    /// The tidal-hifi supplement, for the player it belongs to.
    fn tidal_for(&self, raw: &Raw) -> Option<&Tidal> {
        if raw.identity.eq_ignore_ascii_case("tidal-hifi") { self.tidal.as_ref() } else { None }
    }

    /// Read every player and settle the selection (explicit choice > still playing > newest playing > last).
    async fn snapshot(&self) -> Snapshot {
        let mut players: Vec<(String, Raw)> = Vec::new();
        let mut taken: HashMap<String, u32> = HashMap::new();
        for bus in self.mpris.buses().await {
            let Some(raw) = self.mpris.read(&bus).await else { continue };
            let base = slug(raw.desktop_entry.as_deref().filter(|s| !s.is_empty()).unwrap_or(&raw.identity));
            let n = taken.entry(base.clone()).or_insert(0);
            *n += 1;
            let id = if *n == 1 { base } else { format!("{base}-{n}") };
            players.push((id, raw));
        }

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
        if !exists(&inner.auto) || !inner.playing_since.contains_key(inner.auto.as_ref().unwrap()) {
            let newest = inner.playing_since.iter().max_by_key(|(_, t)| **t).map(|(id, _)| id.clone());
            if let Some(id) = newest {
                inner.auto = Some(id);
            } else if !exists(&inner.auto) {
                inner.auto = ids.first().map(|s| (*s).clone());
            }
        }
        let selected = inner.explicit.clone().or_else(|| inner.auto.clone());
        Snapshot { players, selected }
    }

    fn pick<'a>(snap: &'a Snapshot, player: Option<&str>) -> Result<&'a (String, Raw), ApiError> {
        let want = player.map(str::to_string).or_else(|| snap.selected.clone());
        want.as_ref()
            .and_then(|w| snap.players.iter().find(|(id, _)| id == w))
            .ok_or_else(|| ApiError::new(StatusCode::NOT_FOUND, "no_player", "no such player"))
    }

    pub async fn info(&self) -> Info {
        Info { protocol: 0, name: "music-bridge", version: env!("CARGO_PKG_VERSION") }
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

    pub async fn select(&self, id: &str) -> Result<(), ApiError> {
        let snap = self.snapshot().await;
        Self::pick(&snap, Some(id))?;
        self.inner.lock().unwrap().explicit = Some(id.to_string());
        Ok(())
    }

    pub async fn state(&self, player: Option<&str>) -> Result<State, ApiError> {
        let snap = self.snapshot().await;
        let (id, raw) = Self::pick(&snap, player)?;
        let tid = track_id(raw);
        let cover = raw.meta.art_url.as_ref().map(|url| {
            let key = format!("{:016x}", fnv1a(&format!("{url}|{tid}")));
            self.inner.lock().unwrap().cover_urls.insert(key.clone(), url.clone());
            key
        });

        // Supplement: fills what the MPRIS player lacks. It never overrides MPRIS, and a supplement that does not
        // answer only takes its own capabilities away.
        let sup = match self.tidal_for(raw) {
            Some(t) => t.current().await,
            None => None,
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

        Ok(State {
            player: id.clone(),
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
        })
    }

    pub async fn command(&self, player: Option<&str>, cmd: Cmd) -> Result<(), ApiError> {
        let snap = self.snapshot().await;
        let (_, raw) = Self::pick(&snap, player)?;
        let unsupported = |what: &str| ApiError::new(StatusCode::BAD_REQUEST, "unsupported", format!("{what} is not supported by this player"));
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
                let Some(t) = self.tidal_for(raw) else { return Err(unsupported("shuffle")) };
                return t.set_shuffle(on).await.map_err(|e| ApiError::new(StatusCode::BAD_GATEWAY, "upstream_failed", e));
            }
            Cmd::Repeat(mode) if raw.loop_status.is_some() => Command::Loop(match mode.as_str() {
                "off" => "None",
                "all" => "Playlist",
                "single" => "Track",
                _ => return Err(ApiError::new(StatusCode::BAD_REQUEST, "bad_request", "mode must be off, all or single")),
            }),
            Cmd::Repeat(mode) => {
                if !matches!(mode.as_str(), "off" | "all" | "single") {
                    return Err(ApiError::new(StatusCode::BAD_REQUEST, "bad_request", "mode must be off, all or single"));
                }
                let Some(t) = self.tidal_for(raw) else { return Err(unsupported("repeat")) };
                return t.set_repeat(&mode).await.map_err(|e| ApiError::new(StatusCode::BAD_GATEWAY, "upstream_failed", e));
            }
            Cmd::Open(uri) if !raw.uri_schemes.is_empty() => Command::OpenUri(uri),
            Cmd::Open(_) => return Err(unsupported("open_uri")),
        };
        self.mpris
            .send(raw, c)
            .await
            .map_err(|e| ApiError::new(StatusCode::BAD_GATEWAY, "upstream_failed", e.to_string()))
    }

    pub async fn cover(&self, key: &str, px: u32) -> Result<Arc<Vec<u8>>, ApiError> {
        let (url, cached) = {
            let inner = self.inner.lock().unwrap();
            (inner.cover_urls.get(key).cloned(), inner.cover_cache.get(&(key.to_string(), px)).cloned())
        };
        if let Some(c) = cached {
            return Ok(c);
        }
        let url = url.ok_or_else(|| ApiError::new(StatusCode::NOT_FOUND, "bad_request", "unknown cover key"))?;
        let upstream = |e: String| ApiError::new(StatusCode::BAD_GATEWAY, "upstream_failed", e);
        let bytes = cover::load(&url).await.map_err(upstream)?;
        let jpeg = Arc::new(cover::to_jpeg(bytes, px).await.map_err(upstream)?);

        let mut inner = self.inner.lock().unwrap();
        if inner.cover_cache.len() >= 64 {
            inner.cover_cache.clear(); // small and simple: covers are cheap to rebuild
        }
        inner.cover_cache.insert((key.to_string(), px), jpeg.clone());
        Ok(jpeg)
    }
}
