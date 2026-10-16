mod auth;
mod cover;
mod hub;
mod model;
mod mpris;

use std::sync::Arc;

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

#[tokio::main]
async fn main() {
    tracing_subscriber::fmt().with_max_level(tracing::Level::INFO).init();

    let bind = arg("--bind").unwrap_or_else(|| "0.0.0.0".into());
    let port = arg("--port").unwrap_or_else(|| "47900".into());

    // Access: --no-auth = open, --token X = that token, otherwise a stored token, required unless bound to loopback.
    let required: auth::Required = if flag("--no-auth") {
        None
    } else if let Some(t) = arg("--token") {
        Some(Arc::new(t))
    } else if auth::is_loopback(&bind) {
        None
    } else {
        let (t, path) = auth::load_or_create();
        tracing::info!("access token required: {t}  (stored in {})", path.display());
        Some(Arc::new(t))
    };
    if required.is_none() && !auth::is_loopback(&bind) {
        tracing::warn!("listening on {bind} WITHOUT an access token: anyone on the network can control the players");
    }

    let hub = Hub::new().await.expect("cannot connect to the D-Bus session bus");
    let app = Router::new()
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
        .layer(axum::middleware::from_fn_with_state(required, auth::check))
        .with_state(hub);

    let addr = format!("{bind}:{port}");
    let listener = tokio::net::TcpListener::bind(&addr).await.unwrap_or_else(|e| panic!("cannot listen on {addr}: {e}"));
    tracing::info!("music-bridge listening on http://{addr}/v0");
    axum::serve(listener, app).await.unwrap();
}
