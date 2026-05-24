mod collab;
mod crdt;
mod discovery;
mod guild;
mod ipc;
mod transfer;

use std::sync::Arc;
use tokio::sync::Mutex;
use tokio::net::UnixListener;

use collab::CollabManager;
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
        let _ = std::fs::set_permissions(
            SOCKET_PATH,
            std::fs::Permissions::from_mode(0o666),
        );
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
                        tokio::spawn(async move {
                            if let Err(e) = ipc::handle_connection(stream, gs, fp, cm).await {
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
