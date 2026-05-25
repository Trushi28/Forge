//! mDNS peer discovery — zero-config LAN presence
//!
//! Uses the mdns-sd crate to announce our presence on the local network
//! and discover other Forge users. Service type: `_forge._tcp.local.`

use crate::guild::{GuildPeer, GuildState};
use mdns_sd::{ServiceDaemon, ServiceEvent, ServiceInfo};
use std::sync::Arc;
use std::time::SystemTime;
use tokio::sync::Mutex;

const SERVICE_TYPE: &str = "_forge._tcp.local.";

/// Announce our presence on the LAN via mDNS
pub fn announce(
    handle: &str,
    guild: &str,
    port: u16,
) -> Result<ServiceDaemon, Box<dyn std::error::Error>> {
    let mdns = ServiceDaemon::new()?;

    let host = format!("{}.local.", handle);
    let properties = [("guild", guild), ("handle", handle), ("version", "0.1")];

    let service = ServiceInfo::new(SERVICE_TYPE, handle, &host, "", port, &properties[..])?;

    mdns.register(service)?;
    eprintln!(
        "forge-net: announced as '{}' in guild '{}' on port {}",
        handle, guild, port
    );

    Ok(mdns)
}

/// Run the discovery loop — watches for peers joining/leaving the network
pub async fn run_discovery(
    guild_state: Arc<Mutex<GuildState>>,
) -> Result<(), Box<dyn std::error::Error>> {
    let mdns = ServiceDaemon::new()?;
    let receiver = mdns.browse(SERVICE_TYPE)?;

    eprintln!("forge-net: scanning for peers on LAN...");

    loop {
        match receiver.recv() {
            Ok(event) => {
                match event {
                    ServiceEvent::ServiceResolved(info) => {
                        let handle = info
                            .get_property_val_str("handle")
                            .unwrap_or_default()
                            .to_string();
                        let guild_name = info
                            .get_property_val_str("guild")
                            .unwrap_or_default()
                            .to_string();

                        if handle.is_empty() {
                            continue;
                        }

                        // Don't add ourselves
                        {
                            let gs = guild_state.lock().await;
                            if handle == gs.my_handle {
                                continue;
                            }
                        }

                        let addr = info
                            .get_addresses()
                            .iter()
                            .next()
                            .map(|a| a.to_string())
                            .unwrap_or_default();
                        let port = info.get_port();

                        let peer = GuildPeer {
                            handle: handle.clone(),
                            name: guild_name.clone(),
                            color: "cyan".to_string(),
                            addr: format!("{}:{}", addr, port),
                            last_seen: SystemTime::now(),
                            current_file: String::new(),
                        };

                        {
                            let mut gs = guild_state.lock().await;
                            gs.add_peer(peer);
                        }

                        eprintln!("forge-net: peer online: {} ({})", handle, guild_name);
                    }
                    ServiceEvent::ServiceRemoved(_service_type, fullname) => {
                        // Extract handle from fullname
                        let handle = fullname.split('.').next().unwrap_or("").to_string();

                        if !handle.is_empty() {
                            let mut gs = guild_state.lock().await;
                            gs.remove_peer(&handle);
                            eprintln!("forge-net: peer offline: {}", handle);
                        }
                    }
                    ServiceEvent::SearchStarted(_) => {
                        eprintln!("forge-net: discovery search started");
                    }
                    _ => {}
                }
            }
            Err(e) => {
                eprintln!("forge-net: discovery recv error: {}", e);
                tokio::time::sleep(tokio::time::Duration::from_secs(1)).await;
            }
        }
    }
}
