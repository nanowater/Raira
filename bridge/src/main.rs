mod auth;
mod config;
mod cover;
mod discovery;
mod hub;
mod hubclient;
mod lyrics;
mod model;
mod tidal;
mod tray;
mod ui;
mod mpris;

use std::net::SocketAddr;
use std::sync::{Arc, Mutex};

use axum::extract::{Path, Query, State as AxState};
use axum::http::{StatusCode, header};
use axum::response::IntoResponse;
use axum::routing::{get, post};
use axum::{Json, Router};

use hub::{ApiError, Cmd, Hub};
use model::*;

type Shared = AxState<Arc<Hub>>;
type Res<T> = Result<T, ApiError>;

async fn info(AxState(h): Shared) -> Json<Info> {
    Json(h.info().await)
}

async fn players(AxState(h): Shared) -> Json<Players> {
    Json(h.players().await)
}

async fn select(AxState(h): Shared, Json(b): Json<SelectBody>) -> Res<StatusCode> {
    h.select(&b.id).await.map(|_| StatusCode::NO_CONTENT)
}

async fn state(AxState(h): Shared, Query(q): Query<PlayerQuery>) -> Res<Json<State>> {
    Ok(Json(h.state(q.player.as_deref()).await?))
}

async fn run(h: Arc<Hub>, q: PlayerQuery, cmd: Cmd) -> Res<StatusCode> {
    h.command(q.player.as_deref(), cmd).await.map(|_| StatusCode::NO_CONTENT)
}

async fn playpause(AxState(h): Shared, Query(q): Query<PlayerQuery>) -> Res<StatusCode> {
    run(h, q, Cmd::PlayPause).await
}
async fn play(AxState(h): Shared, Query(q): Query<PlayerQuery>) -> Res<StatusCode> {
    run(h, q, Cmd::Play).await
}
async fn pause(AxState(h): Shared, Query(q): Query<PlayerQuery>) -> Res<StatusCode> {
    run(h, q, Cmd::Pause).await
}
async fn next(AxState(h): Shared, Query(q): Query<PlayerQuery>) -> Res<StatusCode> {
    run(h, q, Cmd::Next).await
}
async fn previous(AxState(h): Shared, Query(q): Query<PlayerQuery>) -> Res<StatusCode> {
    run(h, q, Cmd::Previous).await
}
async fn seek(AxState(h): Shared, Query(q): Query<PlayerQuery>, Json(b): Json<SeekBody>) -> Res<StatusCode> {
    run(h, q, Cmd::SeekMs(b.position_ms)).await
}
async fn volume(AxState(h): Shared, Query(q): Query<PlayerQuery>, Json(b): Json<VolumeBody>) -> Res<StatusCode> {
    run(h, q, Cmd::Volume(b.volume)).await
}
async fn shuffle(AxState(h): Shared, Query(q): Query<PlayerQuery>, Json(b): Json<ShuffleBody>) -> Res<StatusCode> {
    run(h, q, Cmd::Shuffle(b.enabled)).await
}
async fn repeat(AxState(h): Shared, Query(q): Query<PlayerQuery>, Json(b): Json<RepeatBody>) -> Res<StatusCode> {
    run(h, q, Cmd::Repeat(b.mode)).await
}
async fn open(AxState(h): Shared, Query(q): Query<PlayerQuery>, Json(b): Json<OpenBody>) -> Res<StatusCode> {
    run(h, q, Cmd::Open(b.uri)).await
}

async fn cover_img(AxState(h): Shared, Path(key): Path<String>, Query(q): Query<CoverQuery>) -> Res<impl IntoResponse> {
    let px = q.px.unwrap_or(400).clamp(16, 800);
    let jpeg = h.cover(&key, px).await?;
    Ok(([(header::CONTENT_TYPE, "image/jpeg")], jpeg.as_ref().clone()))
}

fn flag(name: &str) -> bool {
    std::env::args().any(|a| a == name)
}

fn arg(name: &str) -> Option<String> {
    let args: Vec<String> = std::env::args().collect();
    args.iter().position(|a| a == name).and_then(|i| args.get(i + 1).cloned())
}

/// `music-bridge --lyrics-test <title> <artists> <album> <seconds>`: one network lookup, the result as JSON on stdout (to compare
/// with the device's own implementation, src/lyrics.cpp).
async fn lyrics_test(a: &[String]) {
    let t = lyrics::Track { title: a[0].clone(), artists: a[1].clone(), album: a[2].clone(), duration_sec: a[3].parse().unwrap_or(0) };
    match lyrics::lookup_network(&t).await {
        Ok(Some(l)) => println!("{}", serde_json::json!({ "synced": l.synced, "lines": l.lines.iter().map(|x| serde_json::json!([x.ms, x.text])).collect::<Vec<_>>() })),
        Ok(None) => println!("{{\"found\":false}}"),
        Err(()) => println!("{{\"error\":true}}"),
    }
}

#[tokio::main]
async fn main() {
    let argv: Vec<String> = std::env::args().collect();
    if let Some(i) = argv.iter().position(|a| a == "--lyrics-test") {
        if argv.len() >= i + 5 {
            lyrics_test(&argv[i + 1..i + 5]).await;
            return;
        }
    }
    tracing_subscriber::fmt().with_max_level(tracing::Level::INFO).init();

    // command line > settings file (edited from the settings page) > defaults
    let cfg = Arc::new(Mutex::new(config::load()));
    let (cfg_bind, cfg_port, cfg_tidal) = {
        let c = cfg.lock().unwrap();
        (c.bind.clone(), c.port, c.tidal_hifi.clone())
    };
    // Listening on this PC only by default: the hub design needs no incoming connection (the bridge connects out), and the
    // settings page and the older v0 API are for this PC. A device that polls this PC (v0) needs --bind 0.0.0.0 (and the token).
    let bind = arg("--bind").or(cfg_bind).unwrap_or_else(|| config::DEFAULT_BIND.into());
    let port: u16 = arg("--port").and_then(|p| p.parse().ok()).or(cfg_port).unwrap_or(config::DEFAULT_PORT);

    // Access: --no-auth = open, --token X = that token, otherwise a stored token, required unless bound to loopback.
    let access = auth::Access::new(&bind, flag("--no-auth"), arg("--token"));
    if access.enforced() {
        tracing::info!("access token required; see the settings page (or {})", config::dir().join("token").display());
    } else if !auth::is_loopback(&bind) {
        tracing::warn!("listening on {bind} WITHOUT an access token: anyone on the network can control the players");
    }

    // tidal-hifi's own API fills what its MPRIS lacks (shuffle, repeat, quality); on by default, harmless when absent
    let tidal_spec = arg("--tidal-hifi").or(cfg_tidal).unwrap_or_else(|| ui::DEFAULT_TIDAL.into());
    let tidal = if flag("--no-tidal-hifi") { None } else { tidal::Tidal::new(&tidal_spec) };
    let hub = Hub::new(tidal).await.expect("cannot connect to the D-Bus session bus");
    hub.set_disabled(cfg.lock().unwrap().disabled_players.clone());

    // the devices' API: /v0, with the token check
    let v0 = Router::new()
        .route("/v0/info", get(info))
        .route("/v0/players", get(players))
        .route("/v0/players/select", post(select))
        .route("/v0/state", get(state))
        .route("/v0/cmd/playpause", post(playpause))
        .route("/v0/cmd/play", post(play))
        .route("/v0/cmd/pause", post(pause))
        .route("/v0/cmd/next", post(next))
        .route("/v0/cmd/previous", post(previous))
        .route("/v0/cmd/seek", post(seek))
        .route("/v0/cmd/volume", post(volume))
        .route("/v0/cmd/shuffle", post(shuffle))
        .route("/v0/cmd/repeat", post(repeat))
        .route("/v0/cmd/open", post(open))
        .route("/v0/cover/{key}", get(cover_img))
        .with_state(hub.clone())
        .layer(axum::middleware::from_fn_with_state(access.clone(), auth::check));

    // the settings page: this PC only
    // the hub client: this bridge connects to devices (from the settings file and --device host[:port])
    let extra: Vec<String> = std::env::args().zip(std::env::args().skip(1)).filter(|(a, _)| a == "--device").map(|(_, v)| v).collect();
    let clients = hubclient::HubClients::start(hub.clone(), cfg.clone(), extra);

    let discovery = discovery::Discovery::start(clients.clone());
    let ui_state: ui::Ui = Arc::new(ui::UiState { hub: hub.clone(), access: access.clone(), bind: bind.clone(), port, config: cfg, clients, discovery });
    let settings = Router::new()
        .route("/", get(ui::page))
        .route("/ui/status", get(ui::status))
        .route("/ui/select", post(ui::select))
        .route("/ui/players/enabled", post(ui::set_player_enabled))
        .route("/ui/token/regenerate", post(ui::regenerate_token))
        .route("/ui/config", post(ui::set_config))
        .route("/ui/devices/add", post(ui::add_device))
        .route("/ui/devices/remove", post(ui::remove_device))
        .route("/ui/devices/pair", post(ui::pair_device))
        .with_state(ui_state.clone())
        .layer(axum::middleware::from_fn_with_state(ui_state, ui::guard));

    let app = v0.merge(settings);
    let addr = format!("{bind}:{port}");
    let listener = tokio::net::TcpListener::bind(&addr).await.unwrap_or_else(|e| panic!("cannot listen on {addr}: {e}"));
    let page = format!("http://127.0.0.1:{port}/");
    tracing::info!("music-bridge listening on http://{addr}/v0 — settings: {page}");

    if !flag("--no-tray") {
        tokio::spawn(tray::spawn(page, hub, access));
    }
    axum::serve(listener, app.into_make_service_with_connect_info::<SocketAddr>()).await.unwrap();
}
