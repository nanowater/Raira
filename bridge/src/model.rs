//! Wire types of the bridge protocol (docs/BRIDGE_PROTOCOL.md, v0).

use serde::{Deserialize, Serialize};

#[derive(Serialize)]
pub struct Info {
    pub protocol: u32,
    pub name: &'static str,
    pub version: &'static str,
}

#[derive(Serialize)]
pub struct PlayerEntry {
    pub id: String,
    pub name: String,
    pub service: &'static str,
    pub status: &'static str,
}

#[derive(Serialize)]
pub struct Players {
    pub selected: Option<String>,
    pub players: Vec<PlayerEntry>,
}

#[derive(Serialize)]
pub struct Track {
    pub id: String,
    pub title: String,
    pub artists: Vec<String>,
    pub album: String,
    pub duration_ms: Option<i64>,
    pub cover: Option<String>,
}

#[derive(Serialize)]
pub struct Quality {
    pub label: String,
    pub badge: String,
}

#[derive(Serialize)]
pub struct State {
    pub player: String,
    pub status: &'static str,
    pub track: Track,
    pub position_ms: Option<i64>,
    pub position_at_ms: u64,
    pub rate: f64,
    pub volume: Option<f64>,
    pub shuffle: Option<bool>,
    pub repeat: Option<&'static str>,
    pub quality: Option<Quality>,
    pub capabilities: Vec<&'static str>,
}

#[derive(Serialize)]
pub struct ErrBody {
    pub error: &'static str,
    pub message: String,
}

#[derive(Deserialize)]
pub struct PlayerQuery {
    pub player: Option<String>,
}

#[derive(Deserialize)]
pub struct SelectBody {
    pub id: String,
}

#[derive(Deserialize)]
pub struct SeekBody {
    pub position_ms: i64,
}

#[derive(Deserialize)]
pub struct VolumeBody {
    pub volume: f64,
}

#[derive(Deserialize)]
pub struct ShuffleBody {
    pub enabled: bool,
}

#[derive(Deserialize)]
pub struct RepeatBody {
    pub mode: String,
}

#[derive(Deserialize)]
pub struct OpenBody {
    pub uri: String,
}

#[derive(Deserialize)]
pub struct CoverQuery {
    pub px: Option<u32>,
}
