//! Encrypted peer-to-peer protocol for Forge guild traffic.

use crate::collab::CollabManager;
use crate::crypto::{ContactBook, EncryptedFrame, Identity, Invite, TrustDecision};
use crate::guild::GuildState;
use crate::ipc::OutgoingMsg;
use crate::transfer::{FilePool, SharedFile};
use serde::{Deserialize, Serialize};
use std::net::{IpAddr, Ipv4Addr, SocketAddr, UdpSocket};
use std::sync::Arc;
use std::time::{SystemTime, UNIX_EPOCH};
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::{TcpListener, TcpStream};
use tokio::sync::{Mutex, broadcast};

const MAX_PLAIN_FRAME: usize = 128 * 1024;
const MAX_ENCRYPTED_FRAME: usize = 64 * 1024 * 1024;
const PROTOCOL_VERSION: u16 = 1;

#[derive(Clone)]
pub struct PeerRuntime {
    pub identity: Arc<Identity>,
    pub contacts: Arc<Mutex<ContactBook>>,
    pub guild_state: Arc<Mutex<GuildState>>,
    pub file_pool: Arc<Mutex<FilePool>>,
    pub collab_mgr: Arc<Mutex<CollabManager>>,
    pub events: broadcast::Sender<OutgoingMsg>,
    pub listen_port: u16,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct PlainHello {
    pub version: u16,
    pub handle: String,
    pub guild: String,
    pub public_key_b64: String,
    pub fingerprint: String,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(tag = "type")]
pub enum PeerMsg {
    #[serde(rename = "PRESENCE")]
    Presence {
        from: String,
        guild: String,
        file: String,
    },

    #[serde(rename = "CHAT")]
    Chat { from: String, text: String },

    #[serde(rename = "DM")]
    Dm {
        from: String,
        to: String,
        text: String,
    },

    #[serde(rename = "PING")]
    Ping {
        from: String,
        file: String,
        line: i32,
    },

    #[serde(rename = "FILE_OFFER")]
    FileOffer {
        from: String,
        name: String,
        data_b64: String,
    },

    #[serde(rename = "FILE_DROP")]
    FileDrop { from: String, name: String },

    #[serde(rename = "COLLAB_REQUEST")]
    CollabRequest {
        session_id: u64,
        from: String,
        file: String,
        initial_text: String,
    },

    #[serde(rename = "COLLAB_ACCEPT")]
    CollabAccept { session_id: u64, from: String },

    #[serde(rename = "COLLAB_DECLINE")]
    CollabDecline { session_id: u64, from: String },

    #[serde(rename = "COLLAB_OP")]
    CollabOp {
        session_id: u64,
        from: String,
        op_json: String,
    },

    #[serde(rename = "CURSOR")]
    Cursor {
        session_id: u64,
        from: String,
        line: i32,
        col: i32,
    },
}

impl PeerRuntime {
    pub async fn invite(
        &self,
        addr: Option<String>,
        relay: Option<String>,
    ) -> Result<String, String> {
        let gs = self.guild_state.lock().await;
        let invite = Invite {
            handle: gs.my_handle.clone(),
            guild: gs.guild_name.clone(),
            public_key_b64: self.identity.public_key_b64(),
            fingerprint: self.identity.fingerprint(),
            addr: addr.or_else(|| local_endpoint(self.listen_port)),
            relay,
        };
        invite.encode()
    }

    pub async fn accept_invite(&self, code: &str) -> Result<Invite, String> {
        let invite = Invite::decode(code)?;
        self.contacts.lock().await.add_invite(invite.clone())?;
        if let Some(addr) = &invite.addr {
            let mut gs = self.guild_state.lock().await;
            gs.add_or_update_peer(
                invite.handle.clone(),
                invite.guild.clone(),
                addr.clone(),
                String::new(),
            );
        }
        if let Some(addr) = &invite.addr {
            let (from, guild) = {
                let gs = self.guild_state.lock().await;
                (gs.my_handle.clone(), gs.guild_name.clone())
            };
            let _ = self
                .send_to_addr(
                    addr,
                    PeerMsg::Presence {
                        from,
                        guild,
                        file: String::new(),
                    },
                )
                .await;
        }
        Ok(invite)
    }

    pub async fn broadcast(&self, msg: PeerMsg) {
        let peers = {
            let gs = self.guild_state.lock().await;
            gs.peers.clone()
        };

        for peer in peers {
            let runtime = self.clone();
            let msg = msg.clone();
            tokio::spawn(async move {
                if let Err(e) = runtime.send_to_addr(&peer.addr, msg).await {
                    eprintln!("forge-net: failed to send to {}: {}", peer.handle, e);
                }
            });
        }
    }

    pub async fn send_to_handle(&self, handle: &str, msg: PeerMsg) -> Result<(), String> {
        let addr = {
            let gs = self.guild_state.lock().await;
            gs.find_peer(handle).map(|p| p.addr.clone())
        };
        let addr = if let Some(addr) = addr {
            addr
        } else {
            self.contacts
                .lock()
                .await
                .get(handle)
                .and_then(|c| c.addr)
                .ok_or_else(|| format!("peer '{handle}' not found"))?
        };
        self.send_to_addr(&addr, msg).await
    }

    pub async fn send_to_addr(&self, addr: &str, msg: PeerMsg) -> Result<(), String> {
        let mut stream = TcpStream::connect(addr).await.map_err(|e| e.to_string())?;
        let hello = self.local_hello().await;
        write_json(&mut stream, &hello, MAX_PLAIN_FRAME).await?;
        let remote: PlainHello = read_json(&mut stream, MAX_PLAIN_FRAME).await?;
        self.verify_remote(&remote, Some(addr.to_string())).await?;

        let mut session = self.identity.session(&remote.public_key_b64)?;
        let body = serde_json::to_vec(&msg).map_err(|e| e.to_string())?;
        let frame = session.encrypt(&body)?;
        write_json(&mut stream, &frame, MAX_ENCRYPTED_FRAME).await?;
        Ok(())
    }

    async fn local_hello(&self) -> PlainHello {
        let gs = self.guild_state.lock().await;
        PlainHello {
            version: PROTOCOL_VERSION,
            handle: gs.my_handle.clone(),
            guild: gs.guild_name.clone(),
            public_key_b64: self.identity.public_key_b64(),
            fingerprint: self.identity.fingerprint(),
        }
    }

    async fn verify_remote(&self, hello: &PlainHello, addr: Option<String>) -> Result<(), String> {
        if hello.version != PROTOCOL_VERSION {
            return Err(format!("unsupported peer protocol {}", hello.version));
        }

        let decision = self.contacts.lock().await.verify_or_trust(
            &hello.handle,
            &hello.guild,
            &hello.public_key_b64,
            addr,
            None,
        )?;

        match decision {
            TrustDecision::Trusted | TrustDecision::NewTrusted => Ok(()),
            TrustDecision::ChangedKey { expected, received } => {
                let _ = self.events.send(OutgoingMsg::TrustWarning {
                    handle: hello.handle.clone(),
                    expected,
                    received: received.clone(),
                });
                Err(format!(
                    "peer '{}' key changed; received {}",
                    hello.handle, received
                ))
            }
        }
    }
}

pub async fn listen(port: u16, runtime: PeerRuntime) -> Result<(), Box<dyn std::error::Error>> {
    let listener = TcpListener::bind(format!("0.0.0.0:{port}")).await?;
    eprintln!("forge-net: encrypted peer listener on port {port}");

    loop {
        let (stream, addr) = listener.accept().await?;
        let runtime = runtime.clone();
        tokio::spawn(async move {
            if let Err(e) = handle_inbound(stream, addr.to_string(), runtime).await {
                eprintln!("forge-net: peer connection failed: {e}");
            }
        });
    }
}

async fn handle_inbound(
    mut stream: TcpStream,
    addr: String,
    runtime: PeerRuntime,
) -> Result<(), String> {
    let remote: PlainHello = read_json(&mut stream, MAX_PLAIN_FRAME).await?;
    let hello = runtime.local_hello().await;
    write_json(&mut stream, &hello, MAX_PLAIN_FRAME).await?;
    runtime.verify_remote(&remote, Some(addr.clone())).await?;

    {
        let mut gs = runtime.guild_state.lock().await;
        gs.add_or_update_peer(
            remote.handle.clone(),
            remote.guild.clone(),
            addr,
            String::new(),
        );
    }

    let session = runtime.identity.session(&remote.public_key_b64)?;

    loop {
        let frame: EncryptedFrame = match read_json(&mut stream, MAX_ENCRYPTED_FRAME).await {
            Ok(frame) => frame,
            Err(_) => return Ok(()),
        };
        let body = session.decrypt(&frame)?;
        let msg: PeerMsg = serde_json::from_slice(&body).map_err(|e| e.to_string())?;
        handle_peer_msg(msg, &runtime).await;
    }
}

async fn handle_peer_msg(msg: PeerMsg, runtime: &PeerRuntime) {
    match msg {
        PeerMsg::Presence { from, guild, file } => {
            runtime.guild_state.lock().await.add_or_update_peer(
                from.clone(),
                guild,
                String::new(),
                file.clone(),
            );
            let _ = runtime
                .events
                .send(OutgoingMsg::PeerOnline { handle: from, file });
        }
        PeerMsg::Chat { from, text } => {
            runtime
                .guild_state
                .lock()
                .await
                .add_chat_message(from.clone(), text.clone());
            let _ = runtime.events.send(OutgoingMsg::ChatRecv { from, text });
        }
        PeerMsg::Dm { from, to: _, text } => {
            runtime
                .guild_state
                .lock()
                .await
                .add_dm(from.clone(), text.clone());
            let _ = runtime.events.send(OutgoingMsg::DmRecv { from, text });
        }
        PeerMsg::Ping { from, file, line } => {
            runtime.guild_state.lock().await.add_ping(
                from.clone(),
                file.clone(),
                line,
                String::new(),
            );
            let _ = runtime
                .events
                .send(OutgoingMsg::PingRecv { from, file, line });
        }
        PeerMsg::FileOffer {
            from,
            name,
            data_b64,
        } => {
            let data = crate::transfer::base64_decode_pub(&data_b64);
            let size = data.len();
            let file = SharedFile {
                name: name.clone(),
                from: from.clone(),
                size,
                timestamp: now_secs(),
                data,
            };
            runtime.file_pool.lock().await.add_file(file);
            runtime
                .guild_state
                .lock()
                .await
                .add_shared_file(name.clone(), from.clone(), size);
            let _ = runtime
                .events
                .send(OutgoingMsg::FileReceived { name, from, size, data_b64: None });
        }
        PeerMsg::FileDrop { from, name } => {
            runtime.file_pool.lock().await.remove_file(&from, &name);
            runtime
                .guild_state
                .lock()
                .await
                .remove_shared_file(&name, &from);
        }
        PeerMsg::CollabRequest {
            session_id,
            from,
            file,
            initial_text: _,
        } => {
            let _ = runtime.events.send(OutgoingMsg::CollabRequest {
                session_id,
                from,
                file,
            });
        }
        PeerMsg::CollabAccept { session_id, from } => {
            let _ = runtime.events.send(OutgoingMsg::CollabAccepted {
                session_id,
                peer: from,
            });
        }
        PeerMsg::CollabDecline { session_id, from } => {
            let _ = runtime.events.send(OutgoingMsg::CollabDeclined {
                session_id,
                peer: from,
            });
        }
        PeerMsg::CollabOp {
            session_id,
            from: _,
            op_json,
        } => {
            // Apply the remote op to the local CRDT session and get the edit position
            let mut cm = runtime.collab_mgr.lock().await;
            let mut edit_info = None;
            if let Some(session) = cm.get_session(session_id) {
                if let Ok(op) = serde_json::from_str(&op_json) {
                    let (pos, is_insert, text_len) = session.apply_remote_get_edit(&op);
                    // Build a simple position-annotated op_json for the C editor
                    let annotated = format!(
                        "{{\"pos\":{},\"is_insert\":{},\"text_len\":{},\"raw\":{}}}",
                        pos,
                        if is_insert { "true" } else { "false" },
                        text_len,
                        op_json
                    );
                    edit_info = Some(annotated);
                }
            }
            drop(cm);

            let final_json = edit_info.unwrap_or(op_json);
            let _ = runtime.events.send(OutgoingMsg::CrdtRemote {
                session_id,
                op_json: final_json,
            });
        }
        PeerMsg::Cursor { .. } => {}
    }
}

async fn read_json<T: for<'de> Deserialize<'de>>(
    stream: &mut TcpStream,
    max_len: usize,
) -> Result<T, String> {
    let mut len_buf = [0u8; 4];
    stream
        .read_exact(&mut len_buf)
        .await
        .map_err(|e| e.to_string())?;
    let len = u32::from_be_bytes(len_buf) as usize;
    if len == 0 || len > max_len {
        return Err("peer frame too large".to_string());
    }
    let mut buf = vec![0u8; len];
    stream
        .read_exact(&mut buf)
        .await
        .map_err(|e| e.to_string())?;
    serde_json::from_slice(&buf).map_err(|e| e.to_string())
}

async fn write_json<T: Serialize>(
    stream: &mut TcpStream,
    value: &T,
    max_len: usize,
) -> Result<(), String> {
    let bytes = serde_json::to_vec(value).map_err(|e| e.to_string())?;
    if bytes.len() > max_len {
        return Err("peer frame too large".to_string());
    }
    let len = (bytes.len() as u32).to_be_bytes();
    stream.write_all(&len).await.map_err(|e| e.to_string())?;
    stream.write_all(&bytes).await.map_err(|e| e.to_string())?;
    Ok(())
}

fn now_secs() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|d| d.as_secs())
        .unwrap_or(0)
}

fn local_endpoint(port: u16) -> Option<String> {
    let socket = UdpSocket::bind(SocketAddr::from((Ipv4Addr::UNSPECIFIED, 0))).ok()?;
    if socket.connect("8.8.8.8:80").is_ok() {
        if let Ok(addr) = socket.local_addr() {
            if !addr.ip().is_unspecified() && !addr.ip().is_loopback() {
                return Some(format!("{}:{port}", addr.ip()));
            }
        }
    }
    Some(format!("{}:{port}", IpAddr::V4(Ipv4Addr::LOCALHOST)))
}
