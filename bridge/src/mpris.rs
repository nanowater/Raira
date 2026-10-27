//! Linux backend: MPRIS over the D-Bus session bus.

use std::collections::HashMap;

use zbus::fdo::DBusProxy;
use zbus::proxy::CacheProperties;
use zbus::{Connection, Proxy};
use zbus::{ObjectPath, OwnedObjectPath, OwnedVariant as OwnedValue};

const PREFIX: &str = "org.mpris.MediaPlayer2.";
const PATH: &str = "/org/mpris/MediaPlayer2";
const ROOT: &str = "org.mpris.MediaPlayer2";
const PLAYER: &str = "org.mpris.MediaPlayer2.Player";

#[derive(Default, Clone)]
pub struct Meta {
    pub title: String,
    pub artists: Vec<String>,
    pub album: String,
    pub length_us: Option<i64>,
    pub art_url: Option<String>,
    pub xesam_url: Option<String>,
    pub trackid: Option<String>,
}

/// Everything read from one player in one go. Optional fields are `None` when the player does not
/// expose the property (tidal-hifi has no Shuffle/LoopStatus).
#[derive(Default, Clone)]
pub struct Raw {
    pub bus: String,
    pub identity: String,
    pub desktop_entry: Option<String>,
    pub status: String,
    pub meta: Meta,
    pub position_us: Option<i64>,
    pub rate: f64,
    pub volume: Option<f64>,
    pub shuffle: Option<bool>,
    pub loop_status: Option<String>,
    pub can_seek: bool,
    pub can_next: bool,
    pub can_prev: bool,
    pub can_control: bool,
    pub uri_schemes: Vec<String>,
}

/// What the bridge can ask a player to do; `Hub::command` maps the protocol's commands onto these.
pub enum Command {
    PlayPause,
    Play,
    Pause,
    Next,
    Previous,
    SeekAbsoluteUs(i64),
    Volume(f64),
    Shuffle(bool),
    Loop(&'static str),
    OpenUri(String),
}

/// The connection to the session bus. Players are found by their bus names, which start with `org.mpris.MediaPlayer2.`.
pub struct Mpris {
    conn: Connection,
}

/// A proxy to one interface of a player. Properties are read live, not cached: a cached value would lag behind the player.
async fn proxy<'a>(
    conn: &'a Connection,
    bus: &'a str,
    iface: &'static str,
) -> zbus::Result<Proxy<'a>> {
    zbus::proxy::Builder::<Proxy>::new(conn)
        .destination(bus)
        .path(PATH)
        .interface(iface)
        .cache_properties(CacheProperties::No)
        .build()
        .await
}

/// Players are not consistent about integer types (some send i64, some u64), so accept both.
fn as_i64(v: &OwnedValue) -> Option<i64> {
    i64::try_from(v)
        .ok()
        .or_else(|| u64::try_from(v).ok().map(|n| n as i64))
}

fn as_string(v: &OwnedValue) -> Option<String> {
    String::try_from(v.clone()).ok()
}

/// The Metadata dictionary as a `Meta`. The album artist stands in when the track has no artist, and the track id is read as
/// an object path or as a plain string, because players differ.
fn parse_meta(m: &HashMap<String, OwnedValue>) -> Meta {
    let mut out = Meta::default();
    if let Some(v) = m.get("xesam:title") {
        out.title = as_string(v).unwrap_or_default();
    }
    if let Some(v) = m.get("xesam:artist") {
        out.artists = Vec::<String>::try_from(v.clone()).unwrap_or_default();
    }
    if out.artists.is_empty()
        && let Some(v) = m.get("xesam:albumArtist")
    {
        out.artists = Vec::<String>::try_from(v.clone()).unwrap_or_default();
    }
    if let Some(v) = m.get("xesam:album") {
        out.album = as_string(v).unwrap_or_default();
    }
    out.length_us = m.get("mpris:length").and_then(as_i64);
    out.art_url = m
        .get("mpris:artUrl")
        .and_then(as_string)
        .filter(|s| !s.is_empty());
    out.xesam_url = m
        .get("xesam:url")
        .and_then(as_string)
        .filter(|s| !s.is_empty());
    out.trackid = m.get("mpris:trackid").and_then(|v| {
        OwnedObjectPath::try_from(v.clone())
            .ok()
            .map(|p| p.to_string())
            .or_else(|| as_string(v))
    });
    out
}

impl Mpris {
    pub async fn connect() -> zbus::Result<Self> {
        Ok(Self {
            conn: Connection::session().await?,
        })
    }

    /// The bus names of all running players, sorted so that their order (and the ids made from it) stays the same.
    pub async fn buses(&self) -> Vec<String> {
        let Ok(dbus) = DBusProxy::new(&self.conn).await else {
            return vec![];
        };
        let mut names: Vec<String> = dbus
            .list_names()
            .await
            .unwrap_or_default()
            .into_iter()
            .map(|n| n.to_string())
            .filter(|n| n.starts_with(PREFIX))
            .collect();
        names.sort();
        names
    }

    /// None when the player vanished or does not answer. Every property is read on its own: only PlaybackStatus is
    /// required, and one that fails (many players have no Shuffle or Volume) stays `None`, which is what hides the control
    /// on the device.
    pub async fn read(&self, bus: &str) -> Option<Raw> {
        let root = proxy(&self.conn, bus, ROOT).await.ok()?;
        let p = proxy(&self.conn, bus, PLAYER).await.ok()?;

        let mut raw = Raw {
            bus: bus.to_string(),
            rate: 1.0,
            ..Default::default()
        };
        raw.identity = root
            .get_property::<String>("Identity")
            .await
            .unwrap_or_else(|_| bus.trim_start_matches(PREFIX).to_string());
        raw.desktop_entry = root.get_property::<String>("DesktopEntry").await.ok();
        raw.uri_schemes = root
            .get_property::<Vec<String>>("SupportedUriSchemes")
            .await
            .unwrap_or_default();

        raw.status = p.get_property::<String>("PlaybackStatus").await.ok()?;
        if let Ok(m) = p
            .get_property::<HashMap<String, OwnedValue>>("Metadata")
            .await
        {
            raw.meta = parse_meta(&m);
        }
        raw.position_us = p.get_property::<i64>("Position").await.ok();
        raw.rate = p.get_property::<f64>("Rate").await.unwrap_or(1.0);
        raw.volume = p.get_property::<f64>("Volume").await.ok();
        raw.shuffle = p.get_property::<bool>("Shuffle").await.ok();
        raw.loop_status = p.get_property::<String>("LoopStatus").await.ok();
        raw.can_seek = p.get_property::<bool>("CanSeek").await.unwrap_or(false);
        raw.can_next = p.get_property::<bool>("CanGoNext").await.unwrap_or(false);
        raw.can_prev = p
            .get_property::<bool>("CanGoPrevious")
            .await
            .unwrap_or(false);
        raw.can_control = p.get_property::<bool>("CanControl").await.unwrap_or(false);
        Some(raw)
    }

    /// Volume, shuffle and loop mode are properties that get written; everything else is a method call.
    pub async fn send(&self, raw: &Raw, cmd: Command) -> zbus::Result<()> {
        let p = proxy(&self.conn, &raw.bus, PLAYER).await?;
        match cmd {
            Command::PlayPause => p.call_method("PlayPause", &()).await.map(drop),
            Command::Play => p.call_method("Play", &()).await.map(drop),
            Command::Pause => p.call_method("Pause", &()).await.map(drop),
            Command::Next => p.call_method("Next", &()).await.map(drop),
            Command::Previous => p.call_method("Previous", &()).await.map(drop),
            Command::SeekAbsoluteUs(us) => {
                // SetPosition needs the current track's object path (a stale one is ignored by the player)
                let path = raw.meta.trackid.as_deref().unwrap_or("/");
                let path = ObjectPath::try_from(path)?;
                p.call_method("SetPosition", &(path, us)).await.map(drop)
            }
            Command::Volume(v) => p.set_property("Volume", v).await.map_err(Into::into),
            Command::Shuffle(on) => p.set_property("Shuffle", on).await.map_err(Into::into),
            Command::Loop(mode) => p.set_property("LoopStatus", mode).await.map_err(Into::into),
            Command::OpenUri(uri) => p.call_method("OpenUri", &(uri,)).await.map(drop),
        }
    }
}
