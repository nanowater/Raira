//! What the bridge needs from the operating system's media players, independent of how they are reached.
//!
//! The hub (`hub.rs`) only talks to a [`PlayerBackend`]. On Linux that is MPRIS over D-Bus (`mpris.rs`); a backend for
//! Windows (the System Media Transport Controls) has to provide the same three things. The data types here keep MPRIS's
//! vocabulary where a value has a fixed set of spellings (see [`Raw::status`] and [`Raw::loop_status`]), so another
//! backend translates its own values to these.

/// The track a player reports.
#[derive(Default, Clone)]
pub struct Meta {
    pub title: String,
    pub artists: Vec<String>,
    pub album: String,
    /// length in microseconds
    pub length_us: Option<i64>,
    /// where the artwork can be fetched: an `http(s)://` or `file://` URL
    pub art_url: Option<String>,
    /// the page or file the track comes from, when the player says (used to recognise Spotify tracks)
    pub xesam_url: Option<String>,
    /// the player's own id of the track, `None` when it has none
    pub trackid: Option<String>,
}

/// Everything read from one player in one go. A value is `None` (or `false` for the `can_*` flags) when the player does not
/// offer it, which is what hides the matching control on the device (tidal-hifi, for one, has no Shuffle or LoopStatus).
#[derive(Default, Clone)]
pub struct Raw {
    /// the backend's own handle for this player: the D-Bus name on Linux. It is what `send` uses to find the player again,
    /// and the hub also looks at it to guess the service.
    pub app_id: String,
    /// the player's display name ("Spotify", "tidal-hifi")
    pub identity: String,
    /// the name of its desktop entry without `.desktop`, when it has one; player ids are made from it
    pub desktop_entry: Option<String>,
    /// `Playing`, `Paused` or `Stopped` (anything else counts as stopped)
    pub status: String,
    pub meta: Meta,
    /// playback position in microseconds
    pub position_us: Option<i64>,
    /// playback rate, 1.0 = normal speed
    pub rate: f64,
    /// 0.0 to 1.0
    pub volume: Option<f64>,
    pub shuffle: Option<bool>,
    /// `None` (off), `Track` (repeat one) or `Playlist` (repeat all)
    pub loop_status: Option<String>,
    pub can_seek: bool,
    pub can_next: bool,
    pub can_prev: bool,
    pub can_control: bool,
    /// the URI schemes the player can open (an empty list means it cannot be told to open anything)
    pub uri_schemes: Vec<String>,
}

/// What the bridge can ask a player to do; `Hub::command` maps the protocol's commands onto these and only sends the ones
/// the player offers.
pub enum Command {
    PlayPause,
    Play,
    Pause,
    Next,
    Previous,
    /// jump to a position, in microseconds from the start of the track
    SeekAbsoluteUs(i64),
    /// 0.0 to 1.0
    Volume(f64),
    Shuffle(bool),
    /// `None`, `Track` or `Playlist`, as in [`Raw::loop_status`]
    Loop(&'static str),
    OpenUri(String),
}

/// A way to reach the players of this system. The hub owns one; errors are plain text because they only end up in a log or
/// in the answer to a command.
#[allow(async_fn_in_trait)] // used with the one concrete backend, never as a trait object
pub trait PlayerBackend: Sized {
    /// Connects to the system's media services. An error here means the bridge cannot work at all.
    async fn connect() -> Result<Self, String>;

    /// Every player that is running now, read in one go. The order must not change between calls while the same players
    /// run, because the hub numbers players with the same name by it. A player that vanishes or does not answer is left
    /// out.
    async fn players(&self) -> Vec<Raw>;

    /// Performs a command on a player returned by [`PlayerBackend::players`].
    async fn send(&self, player: &Raw, cmd: Command) -> Result<(), String>;
}

#[cfg(target_os = "linux")]
pub use crate::mpris::Mpris as Backend;

#[cfg(not(target_os = "linux"))]
compile_error!(
    "no player backend for this platform yet: Linux uses MPRIS (mpris.rs), Windows needs one for the System Media Transport Controls that implements `PlayerBackend`"
);
