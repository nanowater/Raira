//! Supplement for the TIDAL desktop app (tidal-hifi): its MPRIS interface has no shuffle, repeat or audio
//! quality, but its local HTTP API (docs/tidal-hifi-swagger.json) does. Only used for the player whose MPRIS
//! identity is "tidal-hifi" (not, say, TIDAL on a phone mirrored through KDE Connect).

use std::time::Duration;

use serde_json::Value;

const REPEAT_CYCLE: [&str; 3] = ["off", "all", "single"]; // the order tidal-hifi's toggle walks through

pub struct Tidal {
    spec: String,
    base: String,
    http: reqwest::Client,
}

pub struct TidalState {
    pub title: String,
    pub shuffle: Option<bool>,
    pub repeat: Option<&'static str>,
    /// (tier, badge text), e.g. ("HI_RES_LOSSLESS", "24-bit 96.0kHz")
    pub quality: Option<(String, String)>,
}

impl Tidal {
    /// `spec` is "host[:port]" (default port 47836).
    pub fn new(spec: &str) -> Option<Self> {
        let spec = spec
            .trim()
            .trim_start_matches("http://")
            .trim_end_matches('/');
        if spec.is_empty() {
            return None;
        }
        let base = if spec.contains(':') {
            format!("http://{spec}")
        } else {
            format!("http://{spec}:47836")
        };
        // short timeouts: a stalled tidal-hifi must not slow down every /state of the bridge
        let http = reqwest::Client::builder()
            .timeout(Duration::from_millis(900))
            .build()
            .ok()?;
        Some(Self {
            spec: spec.to_string(),
            base,
            http,
        })
    }

    /// "host[:port]" as given.
    pub fn spec(&self) -> &str {
        &self.spec
    }

    /// None when tidal-hifi's API is not reachable or answers nonsense.
    pub async fn current(&self) -> Option<TidalState> {
        let resp = self
            .http
            .get(format!("{}/current", self.base))
            .send()
            .await
            .ok()?;
        if !resp.status().is_success() {
            return None;
        }
        let v: Value = serde_json::from_str(&resp.text().await.ok()?).ok()?;
        let player = &v["player"];
        let quality = v["audioQuality"]["quality"]
            .as_str()
            .filter(|q| !q.is_empty())
            .map(|q| {
                (
                    q.to_string(),
                    v["audioQuality"]["badgeText"]
                        .as_str()
                        .unwrap_or("")
                        .to_string(),
                )
            });
        Some(TidalState {
            title: v["title"].as_str().unwrap_or("").to_string(),
            shuffle: player["shuffle"].as_bool(),
            repeat: player["repeat"]
                .as_str()
                .and_then(|r| REPEAT_CYCLE.iter().copied().find(|c| *c == r)),
            quality,
        })
    }

    async fn post(&self, path: &str) -> Result<(), String> {
        let resp = self
            .http
            .post(format!("{}{path}", self.base))
            .send()
            .await
            .map_err(|e| e.to_string())?;
        if resp.status().is_success() {
            Ok(())
        } else {
            Err(format!("tidal-hifi {path}: HTTP {}", resp.status()))
        }
    }

    /// tidal-hifi only has toggles, so read the state and toggle when it differs.
    pub async fn set_shuffle(&self, on: bool) -> Result<(), String> {
        let cur = self.current().await.ok_or("tidal-hifi is not reachable")?;
        if cur.shuffle == Some(on) {
            return Ok(());
        }
        self.post("/player/shuffle/toggle").await
    }

    /// Walk the off → all → single → off cycle the right number of steps.
    pub async fn set_repeat(&self, mode: &str) -> Result<(), String> {
        let want = REPEAT_CYCLE
            .iter()
            .position(|c| *c == mode)
            .ok_or("mode must be off, all or single")?;
        let cur = self.current().await.ok_or("tidal-hifi is not reachable")?;
        let have = cur
            .repeat
            .and_then(|r| REPEAT_CYCLE.iter().position(|c| *c == r))
            .ok_or("tidal-hifi reports no repeat mode")?;
        let mut at = have;
        for _ in 0..(want + 3 - have) % 3 {
            self.post("/player/repeat/toggle").await?;
            // tidal-hifi reports a change about a second late and computes the next toggle from the state it
            // knows, so a second toggle sent too early is swallowed: wait until this step shows up
            let next = (at + 1) % 3;
            let mut applied = false;
            for _ in 0..25 {
                tokio::time::sleep(Duration::from_millis(100)).await;
                if self
                    .current()
                    .await
                    .is_some_and(|c| c.repeat == Some(REPEAT_CYCLE[next]))
                {
                    applied = true;
                    break;
                }
            }
            if !applied {
                return Err("tidal-hifi did not apply the repeat change".into());
            }
            at = next;
        }
        Ok(())
    }
}
