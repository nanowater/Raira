//! Finds devices on the local network (mDNS / DNS-SD, service `_raira._tcp`, see docs/HUB_PROTOCOL.md). A device
//! that is already configured and shows up at a new address is followed there; the others are offered on the settings
//! page ("Found nearby") for the user to connect to.

use std::collections::HashMap;
use std::net::Ipv4Addr;
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

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

/// How often a found device is asked whether it still answers, and how long it may stay silent before it is hidden.
/// A device that is reset or loses power says no goodbye, and its mDNS records live for over an hour (the PTR record of the
/// ESP32's mDNS has a TTL of 4500 s), so the announcements alone would keep it on the page that long.
const PROBE_EVERY: Duration = Duration::from_secs(4);
const PROBE_TIMEOUT: Duration = Duration::from_millis(900);
const HIDE_AFTER_MISSES: u32 = 2;
/// An entry that has not answered for this long is dropped altogether (it is announced again when it returns).
const DROP_AFTER: Duration = Duration::from_secs(600);

struct Entry {
    found: Found,
    /// probes in a row that went unanswered
    misses: u32,
    down_since: Option<Instant>,
}

pub struct Discovery {
    found: Mutex<HashMap<String, Entry>>, // by full service name
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
        tokio::spawn(probe_loop(me.clone()));
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
                        this.record(
                            info.get_fullname(),
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

    /// A device announced itself: remember it as answering. Only one device lives at an address, so an older entry at
    /// the same address (a device that was reset keeps the address but gets a new id and sometimes a new name) goes.
    fn record(&self, fullname: &str, found: Found) {
        let mut map = self.found.lock().unwrap();
        map.retain(|k, e| k == fullname || e.found.addr != found.addr);
        map.insert(
            fullname.to_string(),
            Entry {
                found,
                misses: 0,
                down_since: None,
            },
        );
    }

    /// The result of one probe of the device behind `fullname`.
    fn probed(&self, fullname: &str, answered: bool) {
        let mut map = self.found.lock().unwrap();
        let Some(e) = map.get_mut(fullname) else {
            return;
        };
        if answered {
            e.misses = 0;
            e.down_since = None;
            return;
        }
        e.misses += 1;
        e.down_since.get_or_insert_with(Instant::now);
        if e.down_since.is_some_and(|t| t.elapsed() > DROP_AFTER) {
            map.remove(fullname);
        }
    }

    /// Found devices that are not configured yet and still answer.
    pub fn nearby(&self, configured: &[DeviceCfg]) -> Vec<Found> {
        let mut v: Vec<Found> = self
            .found
            .lock()
            .unwrap()
            .values()
            .filter(|e| e.misses < HIDE_AFTER_MISSES)
            .map(|e| &e.found)
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

/// Asks every found device now and then whether its port still accepts a connection.
async fn probe_loop(me: Arc<Discovery>) {
    loop {
        tokio::time::sleep(PROBE_EVERY).await;
        let targets: Vec<(String, String)> = me
            .found
            .lock()
            .unwrap()
            .iter()
            .map(|(k, e)| (k.clone(), e.found.addr.clone()))
            .collect();
        for (fullname, addr) in targets {
            let answered = matches!(
                tokio::time::timeout(PROBE_TIMEOUT, tokio::net::TcpStream::connect(&addr)).await,
                Ok(Ok(_))
            );
            me.probed(&fullname, answered);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn disc() -> Discovery {
        Discovery {
            found: Mutex::new(HashMap::new()),
        }
    }

    fn found(id: &str, name: &str, addr: &str) -> Found {
        Found {
            id: id.into(),
            name: name.into(),
            addr: addr.into(),
        }
    }

    #[test]
    fn a_silent_device_is_hidden_and_comes_back_when_it_answers() {
        let d = disc();
        d.record("A._raira._tcp.local.", found("a", "A", "10.0.0.5:18322"));
        assert_eq!(d.nearby(&[]).len(), 1);
        d.probed("A._raira._tcp.local.", false);
        assert_eq!(d.nearby(&[]).len(), 1, "one miss is not enough");
        d.probed("A._raira._tcp.local.", false);
        assert!(d.nearby(&[]).is_empty());
        d.probed("A._raira._tcp.local.", true);
        assert_eq!(d.nearby(&[]).len(), 1);
    }

    #[test]
    fn a_reset_device_replaces_its_old_entry_at_the_same_address() {
        let d = disc();
        d.record(
            "Raira._raira._tcp.local.",
            found("old", "Raira", "10.0.0.5:18322"),
        );
        d.record(
            "Den._raira._tcp.local.",
            found("new", "Den", "10.0.0.5:18322"),
        );
        let list = d.nearby(&[]);
        assert_eq!(list.len(), 1);
        assert_eq!(list[0].id, "new");
    }

    #[test]
    fn two_devices_at_two_addresses_stay_apart_and_configured_ones_are_left_out() {
        let d = disc();
        d.record("A._raira._tcp.local.", found("a", "A", "10.0.0.5:18322"));
        d.record("B._raira._tcp.local.", found("b", "B", "10.0.0.6:18322"));
        assert_eq!(d.nearby(&[]).len(), 2);
        let cfg = DeviceCfg {
            addr: "10.0.0.5:18322".into(),
            ..Default::default()
        };
        let list = d.nearby(&[cfg]);
        assert_eq!(list.len(), 1);
        assert_eq!(list[0].id, "b");
    }
}
