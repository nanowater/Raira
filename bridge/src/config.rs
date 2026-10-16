//! Settings kept in `$XDG_CONFIG_HOME/music-bridge/config.json`, edited from the settings page. Command-line
//! options win over the file; values that are not set fall back to the defaults in `main.rs`.

use std::path::PathBuf;

use serde::{Deserialize, Serialize};

#[derive(Serialize, Deserialize, Default, Clone, Debug)]
pub struct Config {
    pub bind: Option<String>,
    pub port: Option<u16>,
    /// tidal-hifi's address; `Some("")` = turned off, `None` = the default (127.0.0.1:47836)
    pub tidal_hifi: Option<String>,
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
    std::fs::write(path(), serde_json::to_string_pretty(c).unwrap_or_default())
}
