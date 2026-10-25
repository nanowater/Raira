//! Shared-token access control. The bridge exposes playback control and what is playing, so a bridge that is
//! reachable from the network wants a token; one bound to the loopback address does not. The token travels in
//! the clear (plain HTTP): it keeps casual or accidental clients out, it does not stand up to someone sniffing
//! the network.
//!
//! `Access` also keeps a little bookkeeping (when a device last called, how many calls were refused) that the
//! settings page shows, so "wrong token" and "wrong address" can be told apart from the PC side.

use std::net::{IpAddr, SocketAddr};
use std::path::PathBuf;
use std::sync::{Arc, Mutex, RwLock};
use std::time::Instant;

use axum::Json;
use axum::extract::{ConnectInfo, Request, State};
use axum::http::{StatusCode, header};
use axum::middleware::Next;
use axum::response::{IntoResponse, Response};

use crate::config;
use crate::model::ErrBody;

/// 31 symbols, no 0/o/1/i/l: the token is typed on a touch keyboard.
const ALPHABET: &[u8] = b"abcdefghjkmnpqrstuvwxyz23456789";
const TOKEN_LEN: usize = 16;

fn token_path() -> PathBuf {
    config::dir().join("token")
}

fn generate() -> String {
    (0..TOKEN_LEN)
        .map(|_| ALPHABET[rand::random_range(0..ALPHABET.len())] as char)
        .collect()
}

fn store(path: &PathBuf, token: &str) {
    if let Some(dir) = path.parent() {
        let _ = std::fs::create_dir_all(dir);
    }
    #[cfg(unix)]
    {
        use std::io::Write;
        use std::os::unix::fs::OpenOptionsExt;
        if let Ok(mut f) = std::fs::OpenOptions::new()
            .write(true)
            .create(true)
            .truncate(true)
            .mode(0o600)
            .open(path)
        {
            let _ = writeln!(f, "{token}");
        }
    }
    #[cfg(not(unix))]
    let _ = std::fs::write(path, format!("{token}\n"));
}

pub fn is_loopback(bind: &str) -> bool {
    matches!(bind, "127.0.0.1" | "::1" | "localhost") || bind.starts_with("127.")
}

fn same(a: &[u8], b: &[u8]) -> bool {
    // no early exit on the first difference
    a.len() == b.len() && a.iter().zip(b).fold(0u8, |acc, (x, y)| acc | (x ^ y)) == 0
}

#[derive(Default, Clone)]
pub struct Stats {
    pub last_seen: Option<(Instant, IpAddr)>,
    pub requests: u64,
    pub denied: u64,
    pub last_denied: Option<(Instant, IpAddr)>,
}

pub struct Access {
    token: RwLock<String>,
    /// given with --token: not stored and not regenerable
    fixed: bool,
    enforce: bool,
    stats: Mutex<Stats>,
}

impl Access {
    /// Token source: `--token`, else the stored one (created on first use, mode 0600). It is enforced unless
    /// `no_auth`, or the bridge is bound to loopback without an explicit `--token`.
    pub fn new(bind: &str, no_auth: bool, cli_token: Option<String>) -> Arc<Self> {
        let fixed = cli_token.is_some();
        let token = cli_token.unwrap_or_else(|| {
            let path = token_path();
            match std::fs::read_to_string(&path)
                .ok()
                .map(|s| s.trim().to_string())
                .filter(|s| !s.is_empty())
            {
                Some(t) => t,
                None => {
                    let t = generate();
                    store(&path, &t);
                    t
                }
            }
        });
        let enforce = !no_auth && (fixed || !is_loopback(bind));
        Arc::new(Self {
            token: RwLock::new(token),
            fixed,
            enforce,
            stats: Mutex::new(Stats::default()),
        })
    }

    pub fn token(&self) -> String {
        self.token.read().unwrap().clone()
    }
    pub fn enforced(&self) -> bool {
        self.enforce
    }
    pub fn fixed(&self) -> bool {
        self.fixed
    }
    pub fn stats(&self) -> Stats {
        self.stats.lock().unwrap().clone()
    }

    /// A new token (devices have to be given it again). None when the token was fixed on the command line.
    pub fn regenerate(&self) -> Option<String> {
        if self.fixed {
            return None;
        }
        let t = generate();
        store(&token_path(), &t);
        *self.token.write().unwrap() = t.clone();
        Some(t)
    }
}

pub async fn check(
    State(a): State<Arc<Access>>,
    ConnectInfo(peer): ConnectInfo<SocketAddr>,
    req: Request,
    next: Next,
) -> Response {
    let ok = !a.enforce || {
        let given = req
            .headers()
            .get(header::AUTHORIZATION)
            .and_then(|v| v.to_str().ok())
            .and_then(|v| v.strip_prefix("Bearer "))
            .unwrap_or("");
        same(given.as_bytes(), a.token().as_bytes())
    };
    {
        let mut s = a.stats.lock().unwrap();
        if ok {
            s.requests += 1;
            s.last_seen = Some((Instant::now(), peer.ip()));
        } else {
            s.denied += 1;
            s.last_denied = Some((Instant::now(), peer.ip()));
        }
    }
    if ok {
        next.run(req).await
    } else {
        (
            StatusCode::UNAUTHORIZED,
            Json(ErrBody {
                error: "auth_required",
                message: "missing or wrong access token (Authorization: Bearer <token>)".into(),
            }),
        )
            .into_response()
    }
}
