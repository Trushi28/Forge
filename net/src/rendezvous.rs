//! Minimal room-based WebSocket signaling broker for Guild WebRTC sessions.
//!
//! The broker routes only WebRTC offer/answer/ICE signaling. It never carries
//! Guild chat, collaboration, or file payloads. Deploy it behind a TLS reverse
//! proxy and keep the random room token private.

use futures_util::{SinkExt, StreamExt};
use serde::{Deserialize, Serialize};
use std::collections::HashMap;
use std::net::SocketAddr;
use std::sync::Arc;
use tokio::net::{TcpListener, TcpStream};
use tokio::sync::{mpsc, Mutex, Semaphore};
use tokio::time::{timeout, Duration};
use tokio_tungstenite::tungstenite::Message;

const MAX_SIGNAL_BYTES: usize = 64 * 1024;
const MAX_SDP_BYTES: usize = 48 * 1024;
const MAX_CANDIDATE_BYTES: usize = 4096;
const MAX_ROOMS: usize = 4096;
const MAX_ROOM_TOKEN: usize = 128;

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum SignalRole {
    Host,
    Guest,
}

/// Versioned messages exchanged by Guild peers and the rendezvous broker.
#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(tag = "type", rename_all = "snake_case")]
pub enum SignalMessage {
    Join {
        version: u16,
        room: String,
        role: SignalRole,
    },
    PeerJoined {
        role: SignalRole,
    },
    PeerLeft {
        role: SignalRole,
    },
    Offer {
        sdp: String,
    },
    Answer {
        sdp: String,
    },
    Candidate {
        candidate: serde_json::Value,
    },
    Error {
        message: String,
    },
}

#[derive(Default)]
struct Room {
    host: Option<mpsc::Sender<SignalMessage>>,
    guest: Option<mpsc::Sender<SignalMessage>>,
}

type Rooms = Arc<Mutex<HashMap<String, Room>>>;

/// Run a plain WebSocket broker. Put this listener behind a TLS WebSocket
/// proxy before exposing it outside a trusted network.
pub async fn run_server(bind: &str) -> Result<(), Box<dyn std::error::Error + Send + Sync>> {
    let listener = TcpListener::bind(bind).await?;
    let rooms: Rooms = Arc::new(Mutex::new(HashMap::new()));
    eprintln!("forge-net: rendezvous broker listening on {bind}");
    accept_loop(listener, rooms).await
}

async fn accept_loop(
    listener: TcpListener,
    rooms: Rooms,
) -> Result<(), Box<dyn std::error::Error + Send + Sync>> {
    let connections = Arc::new(Semaphore::new(256));
    loop {
        let (stream, addr) = listener.accept().await?;
        let permit = match connections.clone().try_acquire_owned() {
            Ok(permit) => permit,
            Err(_) => {
                drop(stream);
                continue;
            }
        };
        let rooms = rooms.clone();
        tokio::spawn(async move {
            let _permit = permit;
            if let Err(error) = serve_connection(stream, addr, rooms).await {
                eprintln!("forge-net: rendezvous connection {addr} closed: {error}");
            }
        });
    }
}

async fn serve_connection(stream: TcpStream, addr: SocketAddr, rooms: Rooms) -> Result<(), String> {
    let ws = tokio_tungstenite::accept_async(stream)
        .await
        .map_err(|error| error.to_string())?;
    let (mut sink, mut source) = ws.split();
    let first = timeout(Duration::from_secs(10), source.next())
        .await
        .map_err(|_| "signaling join timed out")?
        .ok_or_else(|| "signaling socket closed before join".to_string())?
        .map_err(|error| error.to_string())?;
    let join_text = match first {
        Message::Text(text) => text.to_string(),
        _ => return Err("first signaling frame must be a join message".into()),
    };
    if join_text.len() > MAX_SIGNAL_BYTES {
        return Err("signaling message is too large".into());
    }
    let join: SignalMessage = serde_json::from_str(&join_text).map_err(|e| e.to_string())?;
    let (room_id, role) = match join {
        SignalMessage::Join {
            version: 1,
            room,
            role,
        } => (room, role),
        SignalMessage::Join { version, .. } => {
            return send_error(
                &mut sink,
                format!("unsupported signaling version {version}"),
            )
            .await;
        }
        _ => return Err("expected signaling join message".into()),
    };
    if room_id.len() < 32 || room_id.len() > MAX_ROOM_TOKEN || !room_id.is_ascii() {
        return send_error(&mut sink, "invalid room token".into()).await;
    }

    let (out_tx, mut out_rx) = mpsc::channel::<SignalMessage>(64);
    let registration = {
        let mut map = rooms.lock().await;
        if !map.contains_key(&room_id) && map.len() >= MAX_ROOMS {
            Err("rendezvous capacity reached".to_string())
        } else {
            let room = map.entry(room_id.clone()).or_default();
            let (slot, other, other_role) = match role {
                SignalRole::Host => (&mut room.host, &room.guest, SignalRole::Guest),
                SignalRole::Guest => (&mut room.guest, &room.host, SignalRole::Host),
            };
            if slot.is_some() {
                Err("room already has a peer with this role".to_string())
            } else {
                let existed = other.is_some();
                *slot = Some(out_tx.clone());
                if let Some(other) = other {
                    let _ = other.try_send(SignalMessage::PeerJoined { role });
                }
                Ok(existed.then_some(other_role))
            }
        }
    };
    let existing_role = match registration {
        Ok(existing_role) => existing_role,
        Err(reason) => return send_error(&mut sink, reason).await,
    };
    if let Some(existing_role) = existing_role {
        let _ = out_tx
            .send(SignalMessage::PeerJoined {
                role: existing_role,
            })
            .await;
    }

    let result = async {
    loop {
        tokio::select! {
            incoming = source.next() => {
                let Some(incoming) = incoming else { break; };
                let incoming = incoming.map_err(|e| e.to_string())?;
                let text = match incoming {
                    Message::Text(text) => text.to_string(),
                    Message::Ping(payload) => {
                        sink.send(Message::Pong(payload)).await.map_err(|e| e.to_string())?;
                        continue;
                    }
                    Message::Pong(_) => continue,
                    Message::Close(_) => break,
                    _ => return Err("binary signaling frames are unsupported".into()),
                };
                if text.len() > MAX_SIGNAL_BYTES {
                    return send_error(&mut sink, "signaling message is too large".into()).await;
                }
                let message: SignalMessage = serde_json::from_str(&text).map_err(|e| e.to_string())?;
                if let Err(reason) = validate_signal_payload(&message) {
                    return send_error(&mut sink, reason.into()).await;
                }
                let map = rooms.lock().await;
                let other = map.get(&room_id).and_then(|room| match role {
                    SignalRole::Host => room.guest.as_ref(),
                    SignalRole::Guest => room.host.as_ref(),
                });
                if let Some(other) = other {
                    if other.try_send(message).is_err() {
                        return send_error(&mut sink, "peer signaling queue is full".into()).await;
                    }
                }
            }
            outgoing = out_rx.recv() => {
                let Some(outgoing) = outgoing else { break; };
                let json = serde_json::to_string(&outgoing).map_err(|e| e.to_string())?;
                sink.send(Message::Text(json.into())).await.map_err(|e| e.to_string())?;
            }
        }
    }
    Ok::<(), String>(())
    }.await;

    let mut map = rooms.lock().await;
    if let Some(room) = map.get_mut(&room_id) {
        match role {
            SignalRole::Host => room.host = None,
            SignalRole::Guest => room.guest = None,
        }
        let other = match role {
            SignalRole::Host => room.guest.as_ref(),
            SignalRole::Guest => room.host.as_ref(),
        };
        if let Some(other) = other {
            let _ = other.try_send(SignalMessage::PeerLeft { role });
        }
        if room.host.is_none() && room.guest.is_none() {
            map.remove(&room_id);
        }
    }
    eprintln!("forge-net: rendezvous peer {addr} left room");
    result
}

fn validate_signal_payload(message: &SignalMessage) -> Result<(), &'static str> {
    match message {
        SignalMessage::Offer { sdp } | SignalMessage::Answer { sdp }
            if !sdp.is_empty() && sdp.len() <= MAX_SDP_BYTES =>
        {
            Ok(())
        }
        SignalMessage::Candidate { candidate }
            if candidate.to_string().len() <= MAX_CANDIDATE_BYTES =>
        {
            Ok(())
        }
        SignalMessage::Offer { .. } | SignalMessage::Answer { .. } => {
            Err("invalid or oversized SDP")
        }
        SignalMessage::Candidate { .. } => Err("oversized ICE candidate"),
        _ => Err("message is not valid peer signaling"),
    }
}

async fn send_error<S>(sink: &mut S, message: String) -> Result<(), String>
where
    S: SinkExt<Message> + Unpin,
    S::Error: std::fmt::Display,
{
    let json = serde_json::to_string(&SignalMessage::Error { message })
        .map_err(|error| error.to_string())?;
    sink.send(Message::Text(json.into()))
        .await
        .map_err(|error| error.to_string())?;
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use base64::engine::general_purpose::URL_SAFE_NO_PAD;
    use base64::Engine;
    use futures_util::{SinkExt, StreamExt};
    use tokio_tungstenite::tungstenite::Message;

    #[test]
    fn signaling_payloads_are_bounded_and_versioned() {
        assert!(validate_signal_payload(&SignalMessage::Offer { sdp: "v=0".into() }).is_ok());
        assert!(validate_signal_payload(&SignalMessage::Candidate {
            candidate: serde_json::json!({"candidate": "candidate:1"}),
        })
        .is_ok());
        assert!(validate_signal_payload(&SignalMessage::Offer {
            sdp: "x".repeat(MAX_SDP_BYTES + 1),
        })
        .is_err());
        assert!(validate_signal_payload(&SignalMessage::Join {
            version: 2,
            room: "x".repeat(32),
            role: SignalRole::Host,
        })
        .is_err());
    }

    #[tokio::test]
    async fn broker_forwards_offer_and_candidates_only_to_room_peer() {
        let listener = TcpListener::bind("127.0.0.1:0").await.unwrap();
        let addr = listener.local_addr().unwrap();
        let rooms: Rooms = Arc::new(Mutex::new(HashMap::new()));
        tokio::spawn(async move {
            let _ = accept_loop(listener, rooms).await;
        });
        let url = format!("ws://{addr}");
        let room = URL_SAFE_NO_PAD.encode([7u8; 32]);
        let (mut host, _) = tokio_tungstenite::connect_async(&url).await.unwrap();
        host.send(Message::Text(
            serde_json::to_string(&SignalMessage::Join {
                version: 1,
                room: room.clone(),
                role: SignalRole::Host,
            })
            .unwrap()
            .into(),
        ))
        .await
        .unwrap();
        let (mut guest, _) = tokio_tungstenite::connect_async(&url).await.unwrap();
        guest
            .send(Message::Text(
                serde_json::to_string(&SignalMessage::Join {
                    version: 1,
                    room,
                    role: SignalRole::Guest,
                })
                .unwrap()
                .into(),
            ))
            .await
            .unwrap();
        let host_joined = timeout(Duration::from_secs(2), host.next())
            .await
            .unwrap()
            .unwrap()
            .unwrap();
        let guest_joined = timeout(Duration::from_secs(2), guest.next())
            .await
            .unwrap()
            .unwrap()
            .unwrap();
        assert!(matches!(
            serde_json::from_str::<SignalMessage>(host_joined.to_text().unwrap()).unwrap(),
            SignalMessage::PeerJoined {
                role: SignalRole::Guest
            }
        ));
        assert!(matches!(
            serde_json::from_str::<SignalMessage>(guest_joined.to_text().unwrap()).unwrap(),
            SignalMessage::PeerJoined {
                role: SignalRole::Host
            }
        ));

        let offer = SignalMessage::Offer {
            sdp: "v=0\r\n".into(),
        };
        host.send(Message::Text(serde_json::to_string(&offer).unwrap().into()))
            .await
            .unwrap();
        let forwarded = timeout(Duration::from_secs(2), guest.next())
            .await
            .unwrap()
            .unwrap()
            .unwrap();
        assert!(matches!(
            serde_json::from_str::<SignalMessage>(forwarded.to_text().unwrap()).unwrap(),
            SignalMessage::Offer { .. }
        ));
        host.send(Message::Text(
            serde_json::to_string(&SignalMessage::Candidate {
                candidate: serde_json::json!({"candidate": "candidate:1"}),
            })
            .unwrap()
            .into(),
        ))
        .await
        .unwrap();
        let forwarded_candidate = timeout(Duration::from_secs(2), guest.next())
            .await
            .unwrap()
            .unwrap()
            .unwrap();
        assert!(matches!(
            serde_json::from_str::<SignalMessage>(forwarded_candidate.to_text().unwrap()).unwrap(),
            SignalMessage::Candidate { .. }
        ));
        drop(host);
        drop(guest);
    }
}
