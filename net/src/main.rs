use forge_net::collab;
use forge_net::crypto;
use forge_net::discovery;
use forge_net::guild;
use forge_net::ipc;
use forge_net::peer;
use forge_net::transfer;

use std::path::PathBuf;
use std::sync::Arc;
use tokio::net::UnixListener;
use tokio::sync::{Mutex, broadcast};

use collab::CollabManager;
use crypto::{ContactBook, Identity};
use peer::PeerRuntime;
use transfer::FilePool;


#[tokio::main]
async fn main() {
    eprintln!("forge-net v0.1 starting...");

    // The control socket lives in the per-user config dir (not world-writable /tmp).
    let net_dir = net_config_dir();
    let _ = std::fs::create_dir_all(&net_dir);
    let sock = socket_path();
    if tokio::net::UnixStream::connect(&sock).await.is_ok() {
        eprintln!("forge-net: already running on {}", sock.display());
        return;
    }
    let _ = std::fs::remove_file(&sock);

    // Initialize guild state and load config
    let guild_state = Arc::new(Mutex::new(guild::GuildState::new()));
    {
        let mut gs = guild_state.lock().await;
        gs.load_config();
    }

    // Initialize shared state
    let file_pool = Arc::new(Mutex::new(FilePool::new()));
    let collab_mgr = Arc::new(Mutex::new(CollabManager::new()));
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

    match discovery::announce(
        &handle,
        &guild_name,
        port,
        &identity.public_key_b64(),
        &identity.fingerprint(),
    ) {
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
    let cb = contacts.clone();
    tokio::spawn(async move {
        if let Err(e) = discovery::run_discovery(gs, cb).await {
            eprintln!("forge-net: discovery error: {}", e);
        }
    });

    // NOTE: the old plaintext file-transfer listener was removed: it accepted
    // unauthenticated files. Files travel as encrypted FILE_OFFER peer messages.

    // Start encrypted peer listener in background
    let peer_runtime = PeerRuntime {
        identity: identity.clone(),
        contacts: contacts.clone(),
        guild_state: guild_state.clone(),
        file_pool: file_pool.clone(),
        collab_mgr: collab_mgr.clone(),
        events: events.clone(),
        listen_port: port,
    };
    let peer_listener_runtime = peer_runtime.clone();
    tokio::spawn(async move {
        if let Err(e) = peer::listen(port, peer_listener_runtime).await {
            eprintln!("forge-net: peer listener error: {}", e);
        }
    });

    // Listen for connections from the C editor
    let listener = match UnixListener::bind(&sock) {
        Ok(l) => l,
        Err(e) => {
            eprintln!("forge-net: failed to bind socket {}: {}", sock.display(), e);
            return;
        }
    };

    // Set socket permissions so the editor can connect
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        let _ = std::fs::set_permissions(&sock, std::fs::Permissions::from_mode(0o600));
    }

    eprintln!("forge-net: listening on {}", sock.display());

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
                let _ = std::fs::remove_file(&sock);
                break;
            }
        }
    }
}

fn socket_path() -> PathBuf {
    net_config_dir().join("forge-net.sock")
}

fn net_config_dir() -> PathBuf {
    if let Ok(home) = std::env::var("HOME") {
        PathBuf::from(home).join(".config/forge/net")
    } else {
        PathBuf::from("/tmp/forge-net")
    }
}
