use serde::{Deserialize, Serialize};
use std::sync::Arc;
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::UnixStream;
use tokio::sync::Mutex;

use crate::guild::GuildState;

/// Messages from the C editor to forge-net
#[derive(Debug, Deserialize)]
#[serde(tag = "type")]
pub enum IncomingMsg {
    #[serde(rename = "CHAT_SEND")]
    ChatSend { guild: String, text: String },

    #[serde(rename = "COLLAB_START")]
    CollabStart { peer: String, file: String },

    #[serde(rename = "FILE_SHARE")]
    FileShare {
        name: String,
        data_b64: String,
    },

    #[serde(rename = "PING")]
    Ping {
        target: String,
        file: String,
        line: i32,
    },

    #[serde(rename = "STATUS")]
    Status,
}

/// Messages from forge-net to the C editor
#[derive(Debug, Serialize)]
#[serde(tag = "type")]
pub enum OutgoingMsg {
    #[serde(rename = "CHAT_RECV")]
    ChatRecv { from: String, text: String },

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

    #[serde(rename = "STATUS_RESP")]
    StatusResp {
        peers: Vec<String>,
        guild: String,
    },
}

/// Read a length-prefixed message from the stream
async fn read_message(stream: &mut UnixStream) -> Result<String, Box<dyn std::error::Error>> {
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
    stream: &mut UnixStream,
    msg: &OutgoingMsg,
) -> Result<(), Box<dyn std::error::Error>> {
    let json = serde_json::to_string(msg)?;
    let len = json.len() as u32;
    stream.write_all(&len.to_be_bytes()).await?;
    stream.write_all(json.as_bytes()).await?;
    Ok(())
}

/// Handle a single connection from the C editor
pub async fn handle_connection(
    mut stream: UnixStream,
    guild_state: Arc<Mutex<GuildState>>,
) -> Result<(), Box<dyn std::error::Error>> {
    loop {
        let json = match read_message(&mut stream).await {
            Ok(j) => j,
            Err(_) => return Ok(()), // connection closed
        };

        let msg: IncomingMsg = match serde_json::from_str(&json) {
            Ok(m) => m,
            Err(e) => {
                eprintln!("forge-net: invalid message: {}", e);
                continue;
            }
        };

        match msg {
            IncomingMsg::ChatSend { guild: _, text } => {
                let mut gs = guild_state.lock().await;
                gs.add_chat_message("local".to_string(), text);
            }

            IncomingMsg::CollabStart { peer, file } => {
                eprintln!("forge-net: collab request for {} on {}", peer, file);
                // TODO: establish CRDT session
            }

            IncomingMsg::FileShare { name, data_b64: _ } => {
                eprintln!("forge-net: file shared: {}", name);
                // TODO: store in shared pool
            }

            IncomingMsg::Ping { target, file, line } => {
                eprintln!(
                    "forge-net: ping {} at {}:{}",
                    target, file, line
                );
                // TODO: send to target peer
            }

            IncomingMsg::Status => {
                let gs = guild_state.lock().await;
                let resp = OutgoingMsg::StatusResp {
                    peers: gs.peers.iter().map(|p| p.handle.clone()).collect(),
                    guild: gs.guild_name.clone(),
                };
                write_message(&mut stream, &resp).await?;
            }
        }
    }
}
