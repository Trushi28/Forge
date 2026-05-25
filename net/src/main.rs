mod collab;
mod crdt;
mod crypto;
mod discovery;
mod guild;
mod ipc;
mod peer;
mod transfer;

use std::path::PathBuf;
use std::sync::Arc;
use tokio::net::UnixListener;
use tokio::sync::{Mutex, broadcast};

use collab::CollabManager;
use crypto::{ContactBook, Identity};
use peer::PeerRuntime;
use transfer::FilePool;

const SOCKET_PATH: &str = "/tmp/forge-net.sock";
const TRANSFER_PORT: u16 = 9877;

#[tokio::main]
async fn main() {
    eprintln!("forge-net v0.1 starting...");

    // Clean up old socket
    let _ = std::fs::remove_file(SOCKET_PATH);

    // Initialize guild state and load config
    let guild_state = Arc::new(Mutex::new(guild::GuildState::new()));
    {
        let mut gs = guild_state.lock().await;
        gs.load_config();
    }

    // Initialize shared state
    let file_pool = Arc::new(Mutex::new(FilePool::new()));
    let collab_mgr = Arc::new(Mutex::new(CollabManager::new()));
    let net_dir = net_config_dir();
    let identity = match Identity::load_or_create(&net_dir) {
        Ok(identity) => Arc::new(identity),
        Err(e) => {
            eprintln!("forge-net: failed to load identity: {e}");
            return;
        }
    };
    let contacts = match ContactBook::load(&net_dir) {
        Ok(book) => Arc::new(Mutex::new(book)),
        Err(e) => {
            eprintln!("forge-net: failed to load contacts: {e}");
            return;
        }
    };
    let (events, _) = broadcast::channel(512);
    eprintln!("forge-net: identity fingerprint {}", identity.fingerprint());

    // Start mDNS announcement
    let gs = guild_state.lock().await;
    let handle = gs.my_handle.clone();
    let guild_name = gs.guild_name.clone();
    let port = gs.port;
    drop(gs);

    match discovery::announce(&handle, &guild_name, port) {
        Ok(_mdns) => {
            eprintln!("forge-net: mDNS announced");
            // Keep mdns daemon alive by not dropping it
            // It will be cleaned up when the process exits
            std::mem::forget(_mdns);
        }
        Err(e) => {
            eprintln!("forge-net: mDNS announce failed: {}", e);
        }
    }

    // Start mDNS discovery in background
    let gs = guild_state.clone();
    tokio::spawn(async move {
        if let Err(e) = discovery::run_discovery(gs).await {
            eprintln!("forge-net: discovery error: {}", e);
        }
    });

    // Start file transfer listener in background
    let fp = file_pool.clone();
    tokio::spawn(async move {
        if let Err(e) = transfer::listen_for_transfers(TRANSFER_PORT, fp).await {
            eprintln!("forge-net: transfer listener error: {}", e);
        }
    });

    // Start encrypted peer listener in background
    let peer_runtime = PeerRuntime {
        identity: identity.clone(),
        contacts: contacts.clone(),
        guild_state: guild_state.clone(),
        file_pool: file_pool.clone(),
        collab_mgr: collab_mgr.clone(),
        events: events.clone(),
    };
    let peer_listener_runtime = peer_runtime.clone();
    tokio::spawn(async move {
        if let Err(e) = peer::listen(port, peer_listener_runtime).await {
            eprintln!("forge-net: peer listener error: {}", e);
        }
    });

    // Listen for connections from the C editor
    let listener = match UnixListener::bind(SOCKET_PATH) {
        Ok(l) => l,
        Err(e) => {
            eprintln!("forge-net: failed to bind socket {}: {}", SOCKET_PATH, e);
            return;
        }
    };

    // Set socket permissions so the editor can connect
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        let _ = std::fs::set_permissions(SOCKET_PATH, std::fs::Permissions::from_mode(0o666));
    }

    eprintln!("forge-net: listening on {}", SOCKET_PATH);

    // Handle graceful shutdown
    let shutdown = tokio::signal::ctrl_c();
    tokio::pin!(shutdown);

    loop {
        tokio::select! {
            result = listener.accept() => {
                match result {
                    Ok((stream, _addr)) => {
                        let gs = guild_state.clone();
                        let fp = file_pool.clone();
                        let cm = collab_mgr.clone();
                        let pr = peer_runtime.clone();
                        let ev = events.clone();
                        tokio::spawn(async move {
                            if let Err(e) = ipc::handle_connection(stream, gs, fp, cm, pr, ev).await {
                                eprintln!("forge-net: connection error: {}", e);
                            }
                        });
                    }
                    Err(e) => {
                        eprintln!("forge-net: accept error: {}", e);
                    }
                }
            }
            _ = &mut shutdown => {
                eprintln!("forge-net: shutting down...");
                let _ = std::fs::remove_file(SOCKET_PATH);
                break;
            }
        }
    }
}

fn net_config_dir() -> PathBuf {
    if let Ok(home) = std::env::var("HOME") {
        PathBuf::from(home).join(".config/forge/net")
    } else {
        PathBuf::from("/tmp/forge-net")
    }
}
