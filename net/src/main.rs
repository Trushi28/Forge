mod ipc;
mod guild;
mod discovery;
mod transfer;

use std::sync::Arc;
use tokio::sync::Mutex;
use tokio::net::UnixListener;

const SOCKET_PATH: &str = "/tmp/forge-net.sock";

#[tokio::main]
async fn main() {
    // Clean up old socket
    let _ = std::fs::remove_file(SOCKET_PATH);

    // Initialize guild state
    let guild_state = Arc::new(Mutex::new(guild::GuildState::new()));

    // Start mDNS discovery in background
    let gs = guild_state.clone();
    tokio::spawn(async move {
        if let Err(e) = discovery::run_discovery(gs).await {
            eprintln!("forge-net: discovery error: {}", e);
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

    println!("forge-net: listening on {}", SOCKET_PATH);

    loop {
        match listener.accept().await {
            Ok((stream, _addr)) => {
                let gs = guild_state.clone();
                tokio::spawn(async move {
                    if let Err(e) = ipc::handle_connection(stream, gs).await {
                        eprintln!("forge-net: connection error: {}", e);
                    }
                });
            }
            Err(e) => {
                eprintln!("forge-net: accept error: {}", e);
            }
        }
    }
}
