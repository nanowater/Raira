//! The bridge as a *source* of the hub protocol (docs/HUB_PROTOCOL.md): it connects to devices (ESP32 / simulator),
//! pairs with a code shown on the device, pushes its players' state when it changes and answers the device's commands
//! and cover requests over the same WebSocket. One task per device, reconnecting with back-off.

use std::collections::HashMap;
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

use futures_util::{SinkExt, StreamExt};
use serde::Serialize;
use serde_json::{Value, json};
use tokio::sync::mpsc;
use tokio_tungstenite::tungstenite::Message;

use crate::config::{self, Config, DeviceCfg};
use crate::hub::{Cmd, Hub};

const DEFAULT_PORT: u16 = 47900;
const TICK: Duration = Duration::from_millis(500);
const HEARTBEAT: Duration = Duration::from_secs(30);

#[derive(Clone, Serialize)]
pub struct DeviceStatus {
    pub addr: String,
    pub name: Option<String>,
    /// connecting | pairing_required | connected | offline
    pub state: String,
    pub detail: String,
    pub paired: bool,
}

struct Task {
    handle: tokio::task::JoinHandle<()>,
    pair_tx: mpsc::UnboundedSender<String>,
}

pub struct HubClients {
    hub: Arc<Hub>,
    cfg: Arc<Mutex<Config>>,
    source_id: String,
    source_name: String,
    tasks: Mutex<HashMap<String, Task>>,
    status: Mutex<HashMap<String, DeviceStatus>>,
}

fn random_hex(bytes: usize) -> String {
    (0..bytes).map(|_| format!("{:02x}", rand::random::<u8>())).collect()
}

fn hostname() -> String {
    std::fs::read_to_string("/etc/hostname")
        .ok()
        .map(|s| s.trim().to_string())
        .filter(|s| !s.is_empty())
        .or_else(|| std::env::var("COMPUTERNAME").ok())
        .or_else(|| std::env::var("HOSTNAME").ok())
        .unwrap_or_else(|| "this PC".into())
}

fn normalize(addr: &str) -> Result<String, String> {
    let a = addr.trim().trim_start_matches("ws://").trim_end_matches('/');
    if a.is_empty() || a.contains(' ') || a.contains('/') {
        return Err("the address must look like host or host:port".into());
    }
    match a.rsplit_once(':') {
        Some((h, p)) if !h.is_empty() && p.parse::<u16>().is_ok() => Ok(a.to_string()),
        Some(_) => Err("the address must look like host or host:port".into()),
        None => Ok(format!("{a}:{DEFAULT_PORT}")),
    }
}

impl HubClients {
    /// Makes sure the config has this source's identity, then connects to the configured devices.
    pub fn start(hub: Arc<Hub>, cfg: Arc<Mutex<Config>>, extra_devices: Vec<String>) -> Arc<Self> {
        let (source_id, source_name) = {
            let mut c = cfg.lock().unwrap();
            let id = c.source_id.get_or_insert_with(|| random_hex(16)).clone();
            let name = c.source_name.get_or_insert_with(hostname).clone();
            let _ = config::save(&c);
            (id, name)
        };
        let me = Arc::new(Self { hub, cfg, source_id, source_name, tasks: Mutex::new(HashMap::new()), status: Mutex::new(HashMap::new()) });
        let known: Vec<String> = me.cfg.lock().unwrap().devices.iter().map(|d| d.addr.clone()).collect();
        for addr in known {
            me.spawn(&addr);
        }
        for addr in extra_devices {
            let _ = me.add(&addr);
        }
        me
    }

    fn set_status(&self, addr: &str, state: &str, detail: &str) {
        let (name, paired) = {
            let c = self.cfg.lock().unwrap();
            let d = c.devices.iter().find(|d| d.addr == addr);
            (d.and_then(|d| d.name.clone()), d.is_some_and(|d| d.secret.is_some()))
        };
        self.status
            .lock()
            .unwrap()
            .insert(addr.to_string(), DeviceStatus { addr: addr.to_string(), name, state: state.into(), detail: detail.into(), paired });
    }

    fn spawn(self: &Arc<Self>, addr: &str) {
        let mut tasks = self.tasks.lock().unwrap();
        if tasks.contains_key(addr) {
            return;
        }
        let (pair_tx, pair_rx) = mpsc::unbounded_channel();
        let me = self.clone();
        let a = addr.to_string();
        let handle = tokio::spawn(async move { run_device(me, a, pair_rx).await });
        tasks.insert(addr.to_string(), Task { handle, pair_tx });
    }

    pub fn add(self: &Arc<Self>, addr: &str) -> Result<(), String> {
        let addr = normalize(addr)?;
        {
            let mut c = self.cfg.lock().unwrap();
            if !c.devices.iter().any(|d| d.addr == addr) {
                c.devices.push(DeviceCfg { addr: addr.clone(), name: None, device_id: None, secret: None });
                let _ = config::save(&c);
            }
        }
        self.spawn(&addr);
        Ok(())
    }

    pub fn remove(&self, addr: &str) {
        if let Some(t) = self.tasks.lock().unwrap().remove(addr) {
            t.handle.abort();
        }
        self.status.lock().unwrap().remove(addr);
        let mut c = self.cfg.lock().unwrap();
        c.devices.retain(|d| d.addr != addr);
        let _ = config::save(&c);
    }

    /// Hand a pairing code (typed by the user from the device's screen) to that device's connection.
    pub fn pair(&self, addr: &str, code: &str) -> bool {
        self.tasks.lock().unwrap().get(addr).is_some_and(|t| t.pair_tx.send(code.to_string()).is_ok())
    }

    /// A device announced itself over mDNS. If we know it by id and it moved, follow it to the new address.
    pub fn on_discovered(self: &Arc<Self>, device_id: &str, addr: &str) {
        let old = {
            let mut c = self.cfg.lock().unwrap();
            let Some(d) = c.devices.iter_mut().find(|d| d.device_id.as_deref() == Some(device_id)) else { return };
            if d.addr == addr {
                return;
            }
            let old = std::mem::replace(&mut d.addr, addr.to_string());
            let _ = config::save(&c);
            old
        };
        tracing::info!("device {device_id} moved from {old} to {addr}");
        if let Some(t) = self.tasks.lock().unwrap().remove(&old) {
            t.handle.abort();
        }
        self.status.lock().unwrap().remove(&old);
        self.spawn(addr);
    }

    pub fn configured(&self) -> Vec<DeviceCfg> {
        self.cfg.lock().unwrap().devices.clone()
    }

    pub fn statuses(&self) -> Vec<DeviceStatus> {
        let st = self.status.lock().unwrap();
        let c = self.cfg.lock().unwrap();
        c.devices
            .iter()
            .map(|d| {
                st.get(&d.addr).cloned().unwrap_or(DeviceStatus {
                    addr: d.addr.clone(),
                    name: d.name.clone(),
                    state: "connecting".into(),
                    detail: String::new(),
                    paired: d.secret.is_some(),
                })
            })
            .collect()
    }

    fn secret_of(&self, addr: &str) -> Option<String> {
        self.cfg.lock().unwrap().devices.iter().find(|d| d.addr == addr).and_then(|d| d.secret.clone())
    }

    fn update_device(&self, addr: &str, f: impl FnOnce(&mut DeviceCfg)) {
        let mut c = self.cfg.lock().unwrap();
        if let Some(d) = c.devices.iter_mut().find(|d| d.addr == addr) {
            f(d);
            let _ = config::save(&c);
        }
    }
}

async fn run_device(me: Arc<HubClients>, addr: String, mut pair_rx: mpsc::UnboundedReceiver<String>) {
    let mut backoff = 1u64;
    loop {
        me.set_status(&addr, "connecting", "");
        let started = Instant::now();
        let result = session(&me, &addr, &mut pair_rx).await;
        match result {
            Ok(()) => me.set_status(&addr, "offline", "the device closed the connection"),
            Err(e) => me.set_status(&addr, "offline", &e),
        }
        if started.elapsed() > Duration::from_secs(10) {
            backoff = 1; // it was a real connection, not a failed attempt
        }
        tokio::time::sleep(Duration::from_secs(backoff)).await;
        backoff = (backoff * 2).min(30);
    }
}

fn text(v: Value) -> Message {
    Message::Text(v.to_string().into())
}

/// What was last pushed for one player, to send only changes.
struct Pushed {
    key: String,
    at: Instant,
    position_ms: i64,
    playing: bool,
    rate: f64,
}

async fn session(me: &Arc<HubClients>, addr: &str, pair_rx: &mut mpsc::UnboundedReceiver<String>) -> Result<(), String> {
    let url = format!("ws://{addr}/source");
    let (ws, _) = tokio::time::timeout(Duration::from_secs(5), tokio_tungstenite::connect_async(&url))
        .await
        .map_err(|_| "no answer (is the device on and on the same network?)".to_string())?
        .map_err(|e| e.to_string())?;
    let (mut sink, mut stream) = ws.split();
    let (out_tx, mut out_rx) = mpsc::unbounded_channel::<Message>();

    let _ = out_tx.send(text(json!({
        "t": "hello", "proto": 1, "source_id": me.source_id, "name": me.source_name,
        "os": std::env::consts::OS, "version": env!("CARGO_PKG_VERSION"), "features": ["cover"],
    })));
    if let Some(secret) = me.secret_of(addr) {
        let _ = out_tx.send(text(json!({ "t": "auth", "secret": secret })));
    }

    let mut ready = false;
    let mut pushed_players = String::new();
    let mut pushed: HashMap<String, Pushed> = HashMap::new();
    let mut ticker = tokio::time::interval(TICK);

    loop {
        tokio::select! {
            Some(m) = out_rx.recv() => sink.send(m).await.map_err(|e| e.to_string())?,
            code = pair_rx.recv() => {
                if let Some(code) = code {
                    let _ = out_tx.send(text(json!({ "t": "pair", "code": code })));
                }
            }
            msg = stream.next() => {
                let Some(msg) = msg else { return Err("connection lost".into()) };
                match msg.map_err(|e| e.to_string())? {
                    Message::Text(t) => {
                        let Ok(v) = serde_json::from_str::<Value>(t.as_str()) else { continue };
                        match v["t"].as_str().unwrap_or("") {
                            "welcome" => {
                                let (name, id) = (v["name"].as_str().map(str::to_string), v["device_id"].as_str().map(str::to_string));
                                me.update_device(addr, |d| {
                                    d.name = name;
                                    d.device_id = id;
                                });
                            }
                            "pairing_required" => {
                                if me.secret_of(addr).is_none() {
                                    me.set_status(addr, "pairing_required", "enter the code shown on the device");
                                }
                            }
                            "ready" => {
                                ready = true;
                                pushed_players.clear();
                                pushed.clear();
                                me.set_status(addr, "connected", "");
                            }
                            "paired" => {
                                let secret = v["secret"].as_str().map(str::to_string);
                                me.update_device(addr, |d| d.secret = secret);
                                ready = true;
                                pushed_players.clear();
                                pushed.clear();
                                me.set_status(addr, "connected", "paired");
                            }
                            "pair_failed" => {
                                let why = match v["reason"].as_str() { Some("closed") => "pairing is not open on the device (press Pair a source)", _ => "wrong code" };
                                me.set_status(addr, "pairing_required", why);
                            }
                            "auth_failed" => {
                                me.update_device(addr, |d| d.secret = None); // forgotten by the device: pair again
                                me.set_status(addr, "pairing_required", "the device does not know this PC any more");
                            }
                            "cmd" if ready => {
                                let (me2, tx) = (me.clone(), out_tx.clone());
                                tokio::spawn(async move { tx.send(text(handle_cmd(&me2, &v).await)).ok(); });
                            }
                            "cover_req" if ready => {
                                let (me2, tx) = (me.clone(), out_tx.clone());
                                tokio::spawn(async move { tx.send(handle_cover(&me2, &v).await).ok(); });
                            }
                            _ => {} // focus, lyrics_req (not announced) and the rest
                        }
                    }
                    Message::Close(_) => return Ok(()),
                    _ => {}
                }
            }
            _ = ticker.tick(), if ready => {
                push(me, &out_tx, &mut pushed_players, &mut pushed).await;
            }
        }
    }
}

/// Send the player list and the states that changed (or whose position drifted) since the last push.
async fn push(me: &HubClients, out: &mpsc::UnboundedSender<Message>, players_sent: &mut String, pushed: &mut HashMap<String, Pushed>) {
    let (players, states) = me.hub.all_states().await;
    let list = json!({ "t": "players", "selected": players.selected, "players": players.players }).to_string();
    if list != *players_sent {
        out.send(Message::Text(list.clone().into())).ok();
        *players_sent = list;
    }
    let live: Vec<String> = states.iter().map(|s| s.player.clone()).collect();
    pushed.retain(|id, _| live.contains(id));

    for st in states {
        let mut v = serde_json::to_value(&st).unwrap_or(Value::Null);
        let Some(obj) = v.as_object_mut() else { continue };
        obj.remove("position_at_ms"); // device and PC clocks are not compared: the position is "as of arrival"
        let position = obj.get("position_ms").and_then(Value::as_i64).unwrap_or(0);
        let playing = st.status == "playing";
        let mut keyed = obj.clone();
        keyed.remove("position_ms");
        let key = Value::Object(keyed).to_string();

        let send = match pushed.get(&st.player) {
            None => true,
            Some(p) => {
                let expected = p.position_ms + if p.playing { (p.rate * p.at.elapsed().as_millis() as f64) as i64 } else { 0 };
                p.key != key || (position - expected).abs() > 1000 || p.at.elapsed() >= HEARTBEAT
            }
        };
        if !send {
            continue;
        }
        obj.insert("t".into(), json!("state"));
        out.send(Message::Text(v.to_string().into())).ok();
        pushed.insert(st.player.clone(), Pushed { key, at: Instant::now(), position_ms: position, playing, rate: st.rate });
    }
}

async fn handle_cmd(me: &HubClients, v: &Value) -> Value {
    let id = v["id"].clone();
    let player = v["player"].as_str().unwrap_or("");
    let cmd = match v["op"].as_str().unwrap_or("") {
        "playpause" => Some(Cmd::PlayPause),
        "play" => Some(Cmd::Play),
        "pause" => Some(Cmd::Pause),
        "next" => Some(Cmd::Next),
        "previous" => Some(Cmd::Previous),
        "seek" => v["position_ms"].as_i64().map(Cmd::SeekMs),
        "volume" => v["value"].as_f64().map(Cmd::Volume),
        "shuffle" => v["enabled"].as_bool().map(Cmd::Shuffle),
        "repeat" => v["mode"].as_str().map(|m| Cmd::Repeat(m.to_string())),
        "open" => v["uri"].as_str().map(|u| Cmd::Open(u.to_string())),
        _ => None,
    };
    let Some(cmd) = cmd else {
        return json!({ "t": "ack", "id": id, "ok": false, "error": "bad_request" });
    };
    match me.hub.command(Some(player), cmd).await {
        Ok(()) => json!({ "t": "ack", "id": id, "ok": true }),
        Err(e) => json!({ "t": "ack", "id": id, "ok": false, "error": e.code() }),
    }
}

/// Cover reply: a binary frame `[u32 big-endian request id][JPEG]`, or `cover_err`.
async fn handle_cover(me: &HubClients, v: &Value) -> Message {
    let id = v["id"].as_u64().unwrap_or(0) as u32;
    let key = v["key"].as_str().unwrap_or("");
    let px = v["px"].as_u64().unwrap_or(400).clamp(16, 800) as u32;
    match me.hub.cover(key, px).await {
        Ok(jpeg) => {
            let mut frame = id.to_be_bytes().to_vec();
            frame.extend_from_slice(&jpeg);
            Message::Binary(frame.into())
        }
        Err(_) => text(json!({ "t": "cover_err", "id": id })),
    }
}
