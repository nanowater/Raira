//! Finds devices on the local network (mDNS / DNS-SD, service `_raira._tcp`, see docs/HUB_PROTOCOL.md). A device
//! that is already configured and shows up at a new address is followed there; the others are offered on the settings
//! page ("Found nearby") for the user to connect to.

use std::collections::HashMap;
use std::net::Ipv4Addr;
use std::sync::{Arc, Mutex};

use mdns_sd::{ServiceDaemon, ServiceEvent};
use serde::Serialize;

use crate::config::DeviceCfg;
use crate::hubclient::HubClients;

const SERVICE: &str = "_raira._tcp.local.";

#[derive(Clone, Serialize)]
pub struct Found {
    pub id: String,
    pub name: String,
    /// "ip:port"
    pub addr: String,
}

pub struct Discovery {
    found: Mutex<HashMap<String, Found>>, // by full service name
}

/// Of the addresses a device announced, the one on our own subnet if there is one (a PC often also has Docker or VPN
/// interfaces), else any IPv4 address.
fn pick(addrs: &[Ipv4Addr], ours: Option<Ipv4Addr>) -> Option<Ipv4Addr> {
    let same_net = |a: &Ipv4Addr| ours.is_some_and(|o| a.octets()[..3] == o.octets()[..3]);
    addrs
        .iter()
        .find(|a| same_net(a))
        .or_else(|| {
            addrs
                .iter()
                .find(|a| !a.is_link_local() && !a.is_loopback())
        })
        .copied()
}

impl Discovery {
    pub fn start(clients: Arc<HubClients>) -> Arc<Self> {
        let me = Arc::new(Self {
            found: Mutex::new(HashMap::new()),
        });
        let daemon = match ServiceDaemon::new() {
            Ok(d) => d,
            Err(e) => {
                tracing::warn!("mDNS discovery is off ({e}); add the device's address by hand");
                return me;
            }
        };
        let rx = match daemon.browse(SERVICE) {
            Ok(r) => r,
            Err(e) => {
                tracing::warn!("mDNS browse failed ({e}); add the device's address by hand");
                return me;
            }
        };
        let this = me.clone();
        tokio::spawn(async move {
            let _daemon = daemon; // stops browsing when dropped
            while let Ok(event) = rx.recv_async().await {
                match event {
                    ServiceEvent::ServiceResolved(info) => {
                        let Some(id) = info.get_property_val_str("id").map(str::to_string) else {
                            continue;
                        };
                        let mut addrs: Vec<Ipv4Addr> =
                            info.get_addresses_v4().into_iter().collect();
                        addrs.sort();
                        let ours = crate::ui::lan_ip().and_then(|s| s.parse().ok());
                        let Some(ip) = pick(&addrs, ours) else {
                            continue;
                        };
                        let addr = format!("{ip}:{}", info.get_port());
                        let name = info
                            .get_property_val_str("name")
                            .map(str::to_string)
                            .unwrap_or_else(|| {
                                info.get_fullname()
                                    .split('.')
                                    .next()
                                    .unwrap_or("")
                                    .to_string()
                            });
                        tracing::info!("found device {name} at {addr}");
                        this.found.lock().unwrap().insert(
                            info.get_fullname().to_string(),
                            Found {
                                id: id.clone(),
                                name,
                                addr: addr.clone(),
                            },
                        );
                        clients.on_discovered(&id, &addr);
                    }
                    ServiceEvent::ServiceRemoved(_, fullname) => {
                        this.found.lock().unwrap().remove(&fullname);
                    }
                    _ => {}
                }
            }
        });
        me
    }

    /// Found devices that are not configured yet.
    pub fn nearby(&self, configured: &[DeviceCfg]) -> Vec<Found> {
        let mut v: Vec<Found> = self
            .found
            .lock()
            .unwrap()
            .values()
            .filter(|f| {
                !configured
                    .iter()
                    .any(|d| d.addr == f.addr || d.device_id.as_deref() == Some(f.id.as_str()))
            })
            .cloned()
            .collect();
        v.sort_by(|a, b| a.name.cmp(&b.name));
        v
    }
}
