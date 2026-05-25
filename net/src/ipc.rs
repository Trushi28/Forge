use serde::{Deserialize, Serialize};
use std::sync::Arc;
use tokio::io::{AsyncRead, AsyncReadExt, AsyncWrite, AsyncWriteExt};
use tokio::net::UnixStream;
use tokio::sync::{Mutex, broadcast};

use crate::collab::CollabManager;
use crate::guild::GuildState;
use crate::peer::{PeerMsg, PeerRuntime};
use crate::transfer::FilePool;

/// Messages from the C editor to forge-net
#[derive(Debug, Deserialize)]
#[serde(tag = "type")]
pub enum IncomingMsg {
    #[serde(rename = "CHAT_SEND")]
    ChatSend { guild: String, text: String },

    #[serde(rename = "DM_SEND")]
    DmSend { target: String, text: String },

    #[serde(rename = "INVITE_CREATE")]
    InviteCreate {
        addr: Option<String>,
        relay: Option<String>,
    },

    #[serde(rename = "INVITE_ACCEPT")]
    InviteAccept { code: String },

    #[serde(rename = "TRUST_ACCEPT")]
    TrustAccept { handle: String, fingerprint: String },

    #[serde(rename = "COLLAB_START")]
    CollabStart { peer: String, file: String },

    #[serde(rename = "COLLAB_ACCEPT")]
    CollabAccept { session_id: u64 },

    #[serde(rename = "COLLAB_DECLINE")]
    CollabDecline { session_id: u64 },

    #[serde(rename = "COLLAB_OP")]
    CollabOp { session_id: u64, op_json: String },

    #[serde(rename = "FILE_SHARE")]
    FileShare { name: String, data_b64: String },

    #[serde(rename = "FILE_GRAB")]
    FileGrab { from: String, name: String },

    #[serde(rename = "FILE_DROP")]
    FileDrop { name: String },

    #[serde(rename = "PING")]
    Ping {
        target: String,
        file: String,
        line: i32,
    },

    #[serde(rename = "STATUS")]
    Status,

    #[serde(rename = "GUILD_STATUS")]
    GuildStatus,
}

/// Messages from forge-net to the C editor
#[derive(Debug, Clone, Serialize)]
#[serde(tag = "type")]
pub enum OutgoingMsg {
    #[serde(rename = "CHAT_RECV")]
    ChatRecv { from: String, text: String },

    #[serde(rename = "DM_RECV")]
    DmRecv { from: String, text: String },

    #[serde(rename = "INVITE_CREATED")]
    InviteCreated { code: String, fingerprint: String },

    #[serde(rename = "INVITE_ACCEPTED")]
    InviteAccepted { handle: String, fingerprint: String },

    #[serde(rename = "TRUST_WARNING")]
    TrustWarning {
        handle: String,
        expected: String,
        received: String,
    },

    #[serde(rename = "ERROR")]
    Error { message: String },

    #[serde(rename = "PEER_ONLINE")]
    PeerOnline { handle: String, file: String },

    #[serde(rename = "PEER_OFFLINE")]
    PeerOffline { handle: String },

    #[serde(rename = "PING_RECV")]
    PingRecv {
        from: String,
        file: String,
        line: i32,
    },

    #[serde(rename = "COLLAB_REQUEST")]
    CollabRequest {
        session_id: u64,
        from: String,
        file: String,
    },

    #[serde(rename = "COLLAB_ACCEPTED")]
    CollabAccepted { session_id: u64, peer: String },

    #[serde(rename = "COLLAB_DECLINED")]
    CollabDeclined { session_id: u64, peer: String },

    #[serde(rename = "CRDT_REMOTE")]
    CrdtRemote { session_id: u64, op_json: String },

    #[serde(rename = "FILE_RECEIVED")]
    FileReceived {
        name: String,
        from: String,
        size: usize,
    },

    #[serde(rename = "STATUS_RESP")]
    StatusResp { peers: Vec<String>, guild: String },

    #[serde(rename = "GUILD_STATUS_RESP")]
    GuildStatusResp {
        guild_name: String,
        my_handle: String,
        peer_count: usize,
        peers: Vec<PeerInfo>,
        shared_files: Vec<SharedFileInfo>,
        chat_count: usize,
    },
}

#[derive(Debug, Clone, Serialize)]
pub struct PeerInfo {
    pub handle: String,
    pub addr: String,
    pub current_file: String,
}

#[derive(Debug, Clone, Serialize)]
pub struct SharedFileInfo {
    pub name: String,
    pub from: String,
    pub size: usize,
}

/// Read a length-prefixed message from the stream
type IpcError = Box<dyn std::error::Error + Send + Sync>;

async fn read_message<R>(stream: &mut R) -> Result<String, IpcError>
where
    R: AsyncRead + Unpin,
{
    let mut len_buf = [0u8; 4];
    stream.read_exact(&mut len_buf).await?;
    let len = u32::from_be_bytes(len_buf) as usize;

    if len > 1024 * 1024 {
        return Err("message too large".into());
    }

    let mut msg_buf = vec![0u8; len];
    stream.read_exact(&mut msg_buf).await?;
    Ok(String::from_utf8(msg_buf)?)
}

/// Write a length-prefixed message to the stream
async fn write_message(
    stream: &mut (impl AsyncWrite + Unpin),
    msg: &OutgoingMsg,
) -> Result<(), IpcError> {
    let json = serde_json::to_string(msg)?;
    let len = json.len() as u32;
    stream.write_all(&len.to_be_bytes()).await?;
    stream.write_all(json.as_bytes()).await?;
    Ok(())
}

/// Handle a single connection from the C editor
pub async fn handle_connection(
    stream: UnixStream,
    guild_state: Arc<Mutex<GuildState>>,
    file_pool: Arc<Mutex<FilePool>>,
    collab_mgr: Arc<Mutex<CollabManager>>,
    peer_runtime: PeerRuntime,
    events: broadcast::Sender<OutgoingMsg>,
) -> Result<(), IpcError> {
    let (mut reader, mut writer) = stream.into_split();
    let mut event_rx = events.subscribe();

    loop {
        let json = tokio::select! {
            event = event_rx.recv() => {
                match event {
                    Ok(msg) => {
                        let _ = write_message(&mut writer, &msg).await;
                        continue;
                    }
                    Err(broadcast::error::RecvError::Lagged(_)) => continue,
                    Err(broadcast::error::RecvError::Closed) => return Ok(()),
                }
            }
            read = read_message(&mut reader) => match read {
                Ok(j) => j,
                Err(_) => return Ok(()),
            },
        };

        let msg: IncomingMsg = match serde_json::from_str(&json) {
            Ok(m) => m,
            Err(e) => {
                eprintln!("forge-net: invalid message: {}", e);
                continue;
            }
        };

        match msg {
            IncomingMsg::InviteCreate { addr, relay } => {
                match peer_runtime.invite(addr, relay).await {
                    Ok(code) => {
                        let resp = OutgoingMsg::InviteCreated {
                            code,
                            fingerprint: peer_runtime.identity.fingerprint(),
                        };
                        write_message(&mut writer, &resp).await?;
                    }
                    Err(e) => {
                        write_message(&mut writer, &OutgoingMsg::Error { message: e }).await?;
                    }
                }
            }

            IncomingMsg::InviteAccept { code } => match peer_runtime.accept_invite(&code).await {
                Ok(invite) => {
                    let resp = OutgoingMsg::InviteAccepted {
                        handle: invite.handle,
                        fingerprint: invite.fingerprint,
                    };
                    write_message(&mut writer, &resp).await?;
                }
                Err(e) => {
                    write_message(&mut writer, &OutgoingMsg::Error { message: e }).await?;
                }
            },

            IncomingMsg::TrustAccept {
                handle,
                fingerprint,
            } => {
                eprintln!("forge-net: trust accepted for {handle} ({fingerprint})");
            }

            IncomingMsg::ChatSend { guild: _, text } => {
                let mut gs = guild_state.lock().await;
                let handle = gs.my_handle.clone();
                gs.add_chat_message(handle, text.clone());
                drop(gs);

                peer_runtime
                    .broadcast(PeerMsg::Chat {
                        from: peer_runtime.guild_state.lock().await.my_handle.clone(),
                        text: text.clone(),
                    })
                    .await;
                eprintln!("forge-net: chat: {}", text);
            }

            IncomingMsg::DmSend { target, text } => {
                let handle = guild_state.lock().await.my_handle.clone();
                if let Err(e) = peer_runtime
                    .send_to_handle(
                        &target,
                        PeerMsg::Dm {
                            from: handle,
                            to: target.clone(),
                            text,
                        },
                    )
                    .await
                {
                    write_message(&mut writer, &OutgoingMsg::Error { message: e }).await?;
                }
            }

            IncomingMsg::CollabStart { peer, file } => {
                let gs = guild_state.lock().await;
                if let Some(peer_info) = gs.find_peer(&peer) {
                    let my_handle = gs.my_handle.clone();
                    let mut cm = collab_mgr.lock().await;
                    let session_id = cm.create_session(&file, peer_info, "");
                    eprintln!(
                        "forge-net: started collab session {} with {} on {}",
                        session_id, peer, file
                    );
                    drop(cm);
                    drop(gs);

                    let _ = peer_runtime
                        .send_to_handle(
                            &peer,
                            PeerMsg::CollabRequest {
                                session_id,
                                from: my_handle,
                                file: file.clone(),
                                initial_text: String::new(),
                            },
                        )
                        .await;

                    // Notify the editor
                    let resp = OutgoingMsg::CollabRequest {
                        session_id,
                        from: peer.clone(),
                        file: file.clone(),
                    };
                    let _ = write_message(&mut writer, &resp).await;
                } else {
                    eprintln!("forge-net: peer '{}' not found", peer);
                }
            }

            IncomingMsg::CollabAccept { session_id } => {
                let mut cm = collab_mgr.lock().await;
                if let Some(session) = cm.get_session(session_id) {
                    session.accept();
                    let resp = OutgoingMsg::CollabAccepted {
                        session_id,
                        peer: session.peer_handle.clone(),
                    };
                    let _ = peer_runtime
                        .send_to_handle(
                            &session.peer_handle,
                            PeerMsg::CollabAccept {
                                session_id,
                                from: guild_state.lock().await.my_handle.clone(),
                            },
                        )
                        .await;
                    let _ = write_message(&mut writer, &resp).await;
                    eprintln!("forge-net: collab session {} accepted", session_id);
                }
            }

            IncomingMsg::CollabDecline { session_id } => {
                let mut cm = collab_mgr.lock().await;
                if let Some(session) = cm.get_session(session_id) {
                    let peer = session.peer_handle.clone();
                    session.close();
                    let resp = OutgoingMsg::CollabDeclined { session_id, peer };
                    let _ = write_message(&mut writer, &resp).await;
                    eprintln!("forge-net: collab session {} declined", session_id);
                }
            }

            IncomingMsg::CollabOp {
                session_id,
                op_json,
            } => {
                let mut cm = collab_mgr.lock().await;
                if let Some(session) = cm.get_session(session_id) {
                    let peer = session.peer_handle.clone();
                    if let Ok(op) = serde_json::from_str(&op_json) {
                        session.apply_remote(&op);
                        eprintln!("forge-net: applied CRDT op on session {}", session_id);
                    }
                    drop(cm);
                    let _ = peer_runtime
                        .send_to_handle(
                            &peer,
                            PeerMsg::CollabOp {
                                session_id,
                                from: guild_state.lock().await.my_handle.clone(),
                                op_json,
                            },
                        )
                        .await;
                }
            }

            IncomingMsg::FileShare { name, data_b64 } => {
                let data = crate::transfer::base64_decode_pub(&data_b64);
                let size = data.len();
                let gs = guild_state.lock().await;
                let my_handle = gs.my_handle.clone();
                drop(gs);

                let file = crate::transfer::SharedFile {
                    name: name.clone(),
                    from: my_handle.clone(),
                    size: data.len(),
                    timestamp: std::time::SystemTime::now()
                        .duration_since(std::time::SystemTime::UNIX_EPOCH)
                        .map(|d| d.as_secs())
                        .unwrap_or(0),
                    data,
                };

                let mut pool = file_pool.lock().await;
                pool.add_file(file);

                // Track in guild state
                let mut gs = guild_state.lock().await;
                gs.add_shared_file(name.clone(), my_handle.clone(), size);
                drop(gs);
                drop(pool);

                peer_runtime
                    .broadcast(PeerMsg::FileOffer {
                        from: my_handle,
                        name: name.clone(),
                        data_b64,
                    })
                    .await;

                eprintln!("forge-net: file shared: {}", name);
            }

            IncomingMsg::FileGrab { from, name } => {
                let pool = file_pool.lock().await;
                let key = format!("{}:{}", from, name);
                if let Some(file) = pool.files.get(&key) {
                    let resp = OutgoingMsg::FileReceived {
                        name: file.name.clone(),
                        from: file.from.clone(),
                        size: file.size,
                    };
                    let _ = write_message(&mut writer, &resp).await;
                    eprintln!("forge-net: file grabbed: {}", name);
                } else {
                    eprintln!("forge-net: file not found: {}:{}", from, name);
                }
            }

            IncomingMsg::FileDrop { name } => {
                let gs = guild_state.lock().await;
                let my_handle = gs.my_handle.clone();
                drop(gs);

                let mut pool = file_pool.lock().await;
                pool.remove_file(&my_handle, &name);

                let mut gs = guild_state.lock().await;
                gs.remove_shared_file(&name, &my_handle);
                drop(gs);

                peer_runtime
                    .broadcast(PeerMsg::FileDrop {
                        from: my_handle,
                        name: name.clone(),
                    })
                    .await;

                eprintln!("forge-net: file dropped: {}", name);
            }

            IncomingMsg::Ping { target, file, line } => {
                let gs = guild_state.lock().await;
                let my_handle = gs.my_handle.clone();
                drop(gs);

                let ping = PeerMsg::Ping {
                    from: my_handle.clone(),
                    file: file.clone(),
                    line,
                };
                if target == "all" {
                    peer_runtime.broadcast(ping).await;
                } else if let Err(e) = peer_runtime.send_to_handle(&target, ping).await {
                    write_message(&mut writer, &OutgoingMsg::Error { message: e }).await?;
                }

                // Also notify the local editor of the outgoing ping
                let resp = OutgoingMsg::PingRecv {
                    from: my_handle.clone(),
                    file,
                    line,
                };
                let _ = write_message(&mut writer, &resp).await;
            }

            IncomingMsg::Status => {
                let gs = guild_state.lock().await;
                let resp = OutgoingMsg::StatusResp {
                    peers: gs.peers.iter().map(|p| p.handle.clone()).collect(),
                    guild: gs.guild_name.clone(),
                };
                write_message(&mut writer, &resp).await?;
            }

            IncomingMsg::GuildStatus => {
                let gs = guild_state.lock().await;
                let pool = file_pool.lock().await;

                let peers: Vec<PeerInfo> = gs
                    .peers
                    .iter()
                    .map(|p| PeerInfo {
                        handle: p.handle.clone(),
                        addr: p.addr.clone(),
                        current_file: p.current_file.clone(),
                    })
                    .collect();

                let shared_files: Vec<SharedFileInfo> = pool
                    .files
                    .values()
                    .map(|f| SharedFileInfo {
                        name: f.name.clone(),
                        from: f.from.clone(),
                        size: f.size,
                    })
                    .collect();

                let resp = OutgoingMsg::GuildStatusResp {
                    guild_name: gs.guild_name.clone(),
                    my_handle: gs.my_handle.clone(),
                    peer_count: gs.peers.len(),
                    peers,
                    shared_files,
                    chat_count: gs.chat_history.len(),
                };
                write_message(&mut writer, &resp).await?;
            }
        }
    }
}
