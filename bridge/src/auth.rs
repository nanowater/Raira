//! Shared-token access control. The bridge exposes playback control and what is playing, so a bridge that is
//! reachable from the network wants a token; one bound to the loopback address does not. The token travels in
//! the clear (plain HTTP): it keeps casual or accidental clients out, it does not stand up to someone sniffing
//! the network.

use std::path::PathBuf;
use std::sync::Arc;

use axum::Json;
use axum::extract::{Request, State};
use axum::http::{StatusCode, header};
use axum::middleware::Next;
use axum::response::{IntoResponse, Response};

use crate::model::ErrBody;

/// 31 symbols, no 0/o/1/i/l: the token is typed on a touch keyboard.
const ALPHABET: &[u8] = b"abcdefghjkmnpqrstuvwxyz23456789";
const TOKEN_LEN: usize = 16;

fn token_path() -> PathBuf {
    let base = std::env::var_os("XDG_CONFIG_HOME")
        .map(PathBuf::from)
        .or_else(|| std::env::var_os("HOME").map(|h| PathBuf::from(h).join(".config")))
        .unwrap_or_else(|| PathBuf::from("."));
    base.join("music-bridge").join("token")
}

fn generate() -> String {
    (0..TOKEN_LEN).map(|_| ALPHABET[rand::random_range(0..ALPHABET.len())] as char).collect()
}

/// The stored token, created (mode 0600) on first use.
pub fn load_or_create() -> (String, PathBuf) {
    let path = token_path();
    if let Some(t) = std::fs::read_to_string(&path).ok().map(|s| s.trim().to_string()).filter(|s| !s.is_empty()) {
        return (t, path);
    }
    let token = generate();
    if let Some(dir) = path.parent() {
        let _ = std::fs::create_dir_all(dir);
    }
    #[cfg(unix)]
    {
        use std::io::Write;
        use std::os::unix::fs::OpenOptionsExt;
        if let Ok(mut f) = std::fs::OpenOptions::new().write(true).create(true).truncate(true).mode(0o600).open(&path) {
            let _ = writeln!(f, "{token}");
        }
    }
    #[cfg(not(unix))]
    let _ = std::fs::write(&path, format!("{token}\n"));
    (token, path)
}

pub fn is_loopback(bind: &str) -> bool {
    matches!(bind, "127.0.0.1" | "::1" | "localhost") || bind.starts_with("127.")
}

fn same(a: &[u8], b: &[u8]) -> bool {
    // no early exit on the first difference
    a.len() == b.len() && a.iter().zip(b).fold(0u8, |acc, (x, y)| acc | (x ^ y)) == 0
}

/// `None` = open access.
pub type Required = Option<Arc<String>>;

pub async fn check(State(required): State<Required>, req: Request, next: Next) -> Response {
    let Some(token) = required else { return next.run(req).await };
    let given = req
        .headers()
        .get(header::AUTHORIZATION)
        .and_then(|v| v.to_str().ok())
        .and_then(|v| v.strip_prefix("Bearer "))
        .unwrap_or("");
    if same(given.as_bytes(), token.as_bytes()) {
        next.run(req).await
    } else {
        (
            StatusCode::UNAUTHORIZED,
            Json(ErrBody { error: "auth_required", message: "missing or wrong access token (Authorization: Bearer <token>)".into() }),
        )
            .into_response()
    }
}
