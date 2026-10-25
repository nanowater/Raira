//! System tray icon (Linux: the StatusNotifierItem protocol, so KDE and GNOME with the AppIndicator extension).
//! A menu with a status line, "Open settings" and "Quit". Best effort: without a tray host the bridge just runs
//! without an icon.

use std::sync::Arc;
use std::time::Duration;

use ksni::menu::{MenuItem, StandardItem};
use ksni::{Tray, TrayMethods};

use crate::auth::Access;
use crate::hub::Hub;

struct BridgeTray {
    url: String,
    status: String,
}

fn open_in_browser(url: &str) {
    let _ = std::process::Command::new("xdg-open").arg(url).spawn();
}

impl Tray for BridgeTray {
    fn id(&self) -> String {
        "music-bridge".into()
    }
    fn icon_name(&self) -> String {
        "multimedia-player".into()
    }
    fn title(&self) -> String {
        "Music Bridge".into()
    }
    fn menu(&self) -> Vec<MenuItem<Self>> {
        vec![
            StandardItem {
                label: self.status.clone(),
                enabled: false,
                ..Default::default()
            }
            .into(),
            MenuItem::Separator,
            StandardItem {
                label: "Open settings".into(),
                activate: Box::new(|t: &mut Self| open_in_browser(&t.url)),
                ..Default::default()
            }
            .into(),
            StandardItem {
                label: "Quit".into(),
                activate: Box::new(|_| std::process::exit(0)),
                ..Default::default()
            }
            .into(),
        ]
    }
    // a left click opens the settings, like most tray apps
    fn activate(&mut self, _x: i32, _y: i32) {
        open_in_browser(&self.url);
    }
}

async fn status_line(hub: &Hub, access: &Access) -> String {
    let now = match hub.state(None).await {
        Ok(s) if !s.track.title.is_empty() => format!(
            "{}: {}",
            if s.status == "playing" {
                "Playing"
            } else {
                "Paused"
            },
            s.track.title
        ),
        _ => "No player".to_string(),
    };
    let device = match access.stats().last_seen {
        Some((t, _)) if t.elapsed() < Duration::from_secs(5) => "device connected",
        _ => "no device",
    };
    format!("{now} · {device}")
}

pub async fn spawn(url: String, hub: Arc<Hub>, access: Arc<Access>) {
    let tray = BridgeTray {
        url,
        status: "Starting...".into(),
    };
    let handle = match tray.spawn().await {
        Ok(h) => h,
        Err(e) => {
            tracing::warn!("no tray icon ({e}); open the settings page in a browser instead");
            return;
        }
    };
    loop {
        let line = status_line(&hub, &access).await;
        if handle.update(|t| t.status = line).await.is_none() {
            return; // the tray service is gone
        }
        tokio::time::sleep(Duration::from_secs(2)).await;
    }
}
