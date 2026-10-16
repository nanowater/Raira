//! The settings page: status and settings in a browser at `http://127.0.0.1:<port>/`. Only the PC itself may use
//! it (it shows the access token and changes settings), so `guard` refuses anything that is not a loopback peer
//! with a loopback `Host` (no DNS rebinding) and, for writes, anything without the custom `X-Bridge-UI` header
//! (a web page on another site cannot add it without a CORS preflight, which this server never answers).

use std::net::SocketAddr;
use std::sync::{Arc, Mutex};
use std::time::Instant;

use axum::Json;
use axum::extract::{ConnectInfo, Request, State};
use axum::http::{Method, StatusCode, header};
use axum::middleware::Next;
use axum::response::{Html, IntoResponse, Response};
use serde::Deserialize;
use serde_json::{Value, json};

use crate::auth::Access;
use crate::config::{self, Config};
use crate::hub::{ApiError, Hub};
use crate::hubclient::HubClients;
use crate::tidal::Tidal;

pub struct UiState {
    pub hub: Arc<Hub>,
    pub access: Arc<Access>,
    /// what the server is running with (a saved change only takes effect after a restart)
    pub bind: String,
    pub port: u16,
    pub config: Arc<Mutex<Config>>,
    pub clients: Arc<HubClients>,
}

pub type Ui = Arc<UiState>;

pub const DEFAULT_TIDAL: &str = "127.0.0.1:47836";

pub async fn guard(State(s): State<Ui>, ConnectInfo(peer): ConnectInfo<SocketAddr>, req: Request, next: Next) -> Response {
    let host = req.headers().get(header::HOST).and_then(|h| h.to_str().ok()).unwrap_or("");
    let host_ok = [format!("127.0.0.1:{}", s.port), format!("localhost:{}", s.port), format!("[::1]:{}", s.port)].iter().any(|h| h == host);
    let write_ok = req.method() == Method::GET || req.headers().contains_key("x-bridge-ui");
    if !peer.ip().is_loopback() || !host_ok || !write_ok {
        return (StatusCode::FORBIDDEN, "the settings page is only available on this PC").into_response();
    }
    next.run(req).await
}

pub async fn page() -> Html<&'static str> {
    Html(include_str!("ui.html"))
}

/// The address a device on the same network would use: the IP of the interface the default route leaves by.
fn lan_ip() -> Option<String> {
    let s = std::net::UdpSocket::bind("0.0.0.0:0").ok()?;
    s.connect("192.0.2.1:9").ok()?; // no packet is sent: this only picks the outgoing interface
    Some(s.local_addr().ok()?.ip().to_string())
}

fn ago(t: Option<Instant>) -> Value {
    t.map(|t| json!(t.elapsed().as_secs_f64())).unwrap_or(Value::Null)
}

pub async fn status(State(s): State<Ui>) -> Json<Value> {
    let players = s.hub.players().await;
    let now = s.hub.state(None).await.ok();
    let tidal = s.hub.tidal_status().await;
    let st = s.access.stats();
    let cfg = s.config.lock().unwrap().clone();

    let want_bind = cfg.bind.clone().unwrap_or_else(|| s.bind.clone());
    let want_port = cfg.port.unwrap_or(s.port);
    Json(json!({
        "version": env!("CARGO_PKG_VERSION"),
        "bind": s.bind,
        "port": s.port,
        "lan_ip": lan_ip(),
        "auth": { "enforced": s.access.enforced(), "fixed": s.access.fixed(), "token": s.access.token() },
        "device": {
            "last_seen_s": ago(st.last_seen.map(|(t, _)| t)),
            "ip": st.last_seen.map(|(_, ip)| ip.to_string()),
            "requests": st.requests,
            "denied": st.denied,
            "last_denied_s": ago(st.last_denied.map(|(t, _)| t)),
            "last_denied_ip": st.last_denied.map(|(_, ip)| ip.to_string()),
        },
        "players": players,
        "now": now.map(|n| json!({ "title": n.track.title, "artists": n.track.artists, "status": n.status, "player": n.player })),
        "devices": s.clients.statuses(),
        "tidal": { "enabled": tidal.is_some(), "addr": tidal.as_ref().map(|t| t.0.clone()), "reachable": tidal.map(|t| t.1).unwrap_or(false) },
        "config": {
            "path": config::path().display().to_string(),
            "bind": want_bind,
            "port": want_port,
            "tidal_hifi": cfg.tidal_hifi.unwrap_or_else(|| DEFAULT_TIDAL.into()),
            "restart_needed": want_bind != s.bind || want_port != s.port,
        },
    }))
}

#[derive(Deserialize)]
pub struct SelectBody {
    id: String,
}

pub async fn select(State(s): State<Ui>, Json(b): Json<SelectBody>) -> Result<StatusCode, ApiError> {
    s.hub.select(&b.id).await.map(|_| StatusCode::NO_CONTENT)
}

pub async fn regenerate_token(State(s): State<Ui>) -> Response {
    match s.access.regenerate() {
        Some(_) => StatusCode::NO_CONTENT.into_response(),
        None => (StatusCode::BAD_REQUEST, "the token was set with --token and cannot be changed here").into_response(),
    }
}

#[derive(Deserialize)]
pub struct ConfigBody {
    bind: String,
    port: u16,
    /// "" turns the tidal-hifi supplement off
    tidal_hifi: String,
}

pub async fn set_config(State(s): State<Ui>, Json(b): Json<ConfigBody>) -> Response {
    let bad = |m: &str| (StatusCode::BAD_REQUEST, m.to_string()).into_response();
    let bind = b.bind.trim().to_string();
    if bind != "localhost" && bind.parse::<std::net::IpAddr>().is_err() {
        return bad("the listen address must be an IP address such as 0.0.0.0 or 127.0.0.1");
    }
    if b.port < 1024 {
        return bad("the port must be 1024 or higher");
    }
    let tidal_spec = b.tidal_hifi.trim().to_string();
    let tidal = if tidal_spec.is_empty() { None } else { Some(Tidal::new(&tidal_spec).ok_or(()).map_err(|_| ())) };
    if matches!(tidal, Some(Err(_))) {
        return bad("the tidal-hifi address must look like host or host:port");
    }

    {
        let mut cfg = s.config.lock().unwrap();
        cfg.bind = Some(bind);
        cfg.port = Some(b.port);
        cfg.tidal_hifi = Some(tidal_spec);
        if let Err(e) = config::save(&cfg) {
            return (StatusCode::INTERNAL_SERVER_ERROR, format!("cannot save {}: {e}", config::path().display())).into_response();
        }
    }
    s.hub.set_tidal(tidal.and_then(|t| t.ok())); // applies at once; bind and port need a restart
    StatusCode::NO_CONTENT.into_response()
}

#[derive(Deserialize)]
pub struct AddrBody {
    addr: String,
}

#[derive(Deserialize)]
pub struct PairBody {
    addr: String,
    code: String,
}

pub async fn add_device(State(s): State<Ui>, Json(b): Json<AddrBody>) -> Response {
    match s.clients.add(b.addr.trim()) {
        Ok(()) => StatusCode::NO_CONTENT.into_response(),
        Err(m) => (StatusCode::BAD_REQUEST, m).into_response(),
    }
}

pub async fn remove_device(State(s): State<Ui>, Json(b): Json<AddrBody>) -> StatusCode {
    s.clients.remove(&b.addr);
    StatusCode::NO_CONTENT
}

pub async fn pair_device(State(s): State<Ui>, Json(b): Json<PairBody>) -> Response {
    if s.clients.pair(&b.addr, b.code.trim()) {
        StatusCode::NO_CONTENT.into_response()
    } else {
        (StatusCode::BAD_REQUEST, "that device is not connected right now".to_string()).into_response()
    }
}
