//! Settings kept in `$XDG_CONFIG_HOME/music-bridge/config.json`, edited from the settings page. Command-line
//! options win over the file; values that are not set fall back to the defaults in `main.rs`.

use std::path::PathBuf;

use serde::{Deserialize, Serialize};

/// What the bridge listens on when neither the command line nor the settings file says otherwise. The settings page stores bind and
/// port only when they differ from these, so a later change of a default reaches everyone who never chose a value.
pub const DEFAULT_BIND: &str = "127.0.0.1";
pub const DEFAULT_PORT: u16 = 18801;

#[derive(Serialize, Deserialize, Default, Clone, Debug)]
pub struct Config {
    pub bind: Option<String>,
    pub port: Option<u16>,
    /// tidal-hifi's address; `Some("")` = turned off, `None` = the default (127.0.0.1:47836)
    pub tidal_hifi: Option<String>,
    /// this bridge's identity towards devices (generated on first use)
    pub source_id: Option<String>,
    pub source_name: Option<String>,
    /// the devices (hubs) this bridge connects to
    #[serde(default)]
    pub devices: Vec<DeviceCfg>,
}

#[derive(Serialize, Deserialize, Clone, Debug, Default)]
pub struct DeviceCfg {
    /// "host:port"
    pub addr: String,
    pub name: Option<String>,
    /// the device's own id (from its welcome / mDNS record): lets us follow it when its address changes
    #[serde(default)]
    pub device_id: Option<String>,
    /// from pairing; stays on this PC
    pub secret: Option<String>,
}

pub fn dir() -> PathBuf {
    let base = std::env::var_os("XDG_CONFIG_HOME")
        .map(PathBuf::from)
        .or_else(|| std::env::var_os("HOME").map(|h| PathBuf::from(h).join(".config")))
        .unwrap_or_else(|| PathBuf::from("."));
    base.join("music-bridge")
}

pub fn path() -> PathBuf {
    dir().join("config.json")
}

pub fn load() -> Config {
    std::fs::read_to_string(path()).ok().and_then(|s| serde_json::from_str(&s).ok()).unwrap_or_default()
}

pub fn save(c: &Config) -> std::io::Result<()> {
    std::fs::create_dir_all(dir())?;
    let text = serde_json::to_string_pretty(c).unwrap_or_default();
    // it holds the pairing secrets of the devices: readable by this user only
    #[cfg(unix)]
    {
        use std::io::Write;
        use std::os::unix::fs::{OpenOptionsExt, PermissionsExt};
        let mut f = std::fs::OpenOptions::new().write(true).create(true).truncate(true).mode(0o600).open(path())?;
        f.write_all(text.as_bytes())?;
        let _ = std::fs::set_permissions(path(), std::fs::Permissions::from_mode(0o600)); // a file created earlier keeps its old mode
        Ok(())
    }
    #[cfg(not(unix))]
    std::fs::write(path(), text)
}
