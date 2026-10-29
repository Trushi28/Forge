//! Guild peer transport over WebRTC data channels.
//!
//! WebSocket signaling is only used to exchange SDP and ICE candidates. All
//! application frames stay on the data channel and retain Forge's identity DH
//! and XChaCha20-Poly1305 framing on top of DTLS.

use crate::crypto::EncryptedFrame;
use crate::peer::{PeerMsg, PeerRuntime, PlainHello, RtcPeer};
use crate::rendezvous::{SignalMessage, SignalRole};
use async_trait::async_trait;
use futures_util::{SinkExt, StreamExt};
use serde::{Deserialize, Serialize};
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::Arc;
use tokio::sync::{mpsc, oneshot, Mutex};
use tokio::time::Duration;
use tokio_tungstenite::tungstenite::Message;
use webrtc::data_channel::{DataChannel, DataChannelEvent};
use webrtc::peer_connection::RTCIceCandidateInit;
use webrtc::peer_connection::{
    PeerConnection, PeerConnectionBuilder, PeerConnectionEventHandler, RTCConfigurationBuilder,
    RTCIceServer, RTCPeerConnectionIceEvent,
};

const MAX_DATA_CHANNEL_FRAME: usize = 16 * 1024;
const MAX_REMOTE_HELLO: usize = 8 * 1024;
pub(crate) const RTC_MAX_PEER_MESSAGE: usize = 10 * 1024 * 1024;
const RTC_CHUNK_BYTES: usize = 8 * 1024;
const RTC_MAX_CHUNKS: usize = RTC_MAX_PEER_MESSAGE / RTC_CHUNK_BYTES;
static NEXT_MESSAGE_ID: AtomicU64 = AtomicU64::new(1);

pub(crate) fn next_message_id() -> u64 {
    NEXT_MESSAGE_ID.fetch_add(1, Ordering::Relaxed)
}

#[derive(Debug, Serialize, Deserialize)]
#[serde(tag = "type", rename_all = "snake_case")]
pub(crate) enum PeerWireFrame {
    Hello(PlainHello),
    Encrypted(EncryptedFrame),
    EncryptedChunk {
        message_id: u64,
        index: u16,
        total: u16,
        frame: EncryptedFrame,
    },
}

struct ChunkAssembly {
    message_id: u64,
    total: u16,
    next_index: u16,
    bytes: Vec<u8>,
}

#[derive(Clone)]
struct RtcHandler {
    signals: mpsc::Sender<SignalMessage>,
    channels: mpsc::Sender<Arc<dyn DataChannel>>,
}

#[async_trait]
impl PeerConnectionEventHandler for RtcHandler {
    async fn on_ice_candidate(&self, event: RTCPeerConnectionIceEvent) {
        if let Ok(candidate) = event.candidate.to_json() {
            let candidate = match serde_json::to_value(candidate) {
                Ok(candidate) => candidate,
                Err(_) => return,
            };
            let _ = self
                .signals
                .try_send(SignalMessage::Candidate { candidate });
        }
    }

    async fn on_data_channel(&self, channel: Arc<dyn DataChannel>) {
        let _ = self.channels.try_send(channel);
    }
}

/// Join a room and establish one ICE-negotiated, fingerprint-checked peer.
/// `as_guest` is true for the person accepting the invite.
pub async fn run(
    runtime: PeerRuntime,
    signal_url: String,
    room: String,
    as_guest: bool,
) -> Result<(), String> {
    let role = if as_guest {
        SignalRole::Guest
    } else {
        SignalRole::Host
    };
    let (socket, _) = tokio_tungstenite::connect_async(&signal_url)
        .await
        .map_err(|error| format!("connect rendezvous: {error}"))?;
    let (mut sink, source) = socket.split();
    let join = SignalMessage::Join {
        version: 1,
        room,
        role,
    };
    let json = serde_json::to_string(&join).map_err(|error| error.to_string())?;
    sink.send(Message::Text(json.into()))
        .await
        .map_err(|error| format!("send rendezvous join: {error}"))?;

    let (signal_tx, mut signal_rx) = mpsc::channel::<SignalMessage>(64);
    let (channel_tx, mut channel_rx) = mpsc::channel::<Arc<dyn DataChannel>>(4);
    let mut ws_source = source;
    let mut ws_sink = sink;
    let (writer_tx, mut writer_rx) = mpsc::channel::<SignalMessage>(64);
    let signal_forward = signal_tx.clone();
    let reader_task = tokio::spawn(async move {
        while let Some(message) = ws_source.next().await {
            let message = match message {
                Ok(Message::Text(text)) => text.to_string(),
                Ok(Message::Ping(_)) | Ok(Message::Pong(_)) => continue,
                Ok(Message::Close(_)) => break,
                Ok(_) => continue,
                Err(error) => {
                    let _ = signal_forward
                        .send(SignalMessage::Error {
                            message: format!("rendezvous read failed: {error}"),
                        })
                        .await;
                    break;
                }
            };
            if message.len() > 64 * 1024 {
                break;
            }
            let Ok(parsed) = serde_json::from_str::<SignalMessage>(&message) else {
                continue;
            };
            if signal_forward.send(parsed).await.is_err() {
                break;
            }
        }
    });
    let writer_task = tokio::spawn(async move {
        while let Some(signal) = writer_rx.recv().await {
            let Ok(json) = serde_json::to_string(&signal) else {
                break;
            };
            if ws_sink.send(Message::Text(json.into())).await.is_err() {
                break;
            }
        }
    });
    // Reuse one bounded sender for ICE callback messages and SDP responses.
    let handler = Arc::new(RtcHandler {
        signals: writer_tx.clone(),
        channels: channel_tx.clone(),
    });
    let ice_servers = configured_ice_servers()?;
    let pc = PeerConnectionBuilder::new()
        .with_configuration(
            RTCConfigurationBuilder::default()
                .with_ice_servers(ice_servers)
                .build(),
        )
        .with_handler(handler)
        .with_udp_addrs(vec!["0.0.0.0:0"])
        .build()
        .await
        .map_err(|error| format!("create peer connection: {error}"))?;

    let mut data_channel = None;
    if !as_guest {
        let channel = pc
            .create_data_channel("forge-guild", None)
            .await
            .map_err(|error| format!("create data channel: {error}"))?;
        let _ = channel_tx.send(channel).await;
    }

    let mut remote_description_set = false;
    let mut queued_candidates: Vec<RTCIceCandidateInit> = Vec::new();
    let (connected_tx, mut connected_rx) = oneshot::channel::<()>();
    let mut connected_tx = Some(connected_tx);
    let mut channel_started = false;
    let mut got_remote_peer = false;
    let mut last_channel_task = None;

    let negotiation_deadline = tokio::time::Instant::now() + Duration::from_secs(120);
    loop {
        if tokio::time::Instant::now() >= negotiation_deadline {
            return Err("ICE negotiation timed out".into());
        }
        tokio::select! {
            signal = signal_rx.recv() => {
                let Some(signal) = signal else { break; };
                match signal {
                    SignalMessage::PeerJoined { role: SignalRole::Guest } if !as_guest && !got_remote_peer => {
                        got_remote_peer = true;
                        let offer = pc.create_offer(None).await.map_err(|e| e.to_string())?;
                        pc.set_local_description(offer.clone()).await.map_err(|e| e.to_string())?;
                        writer_tx.send(SignalMessage::Offer {
                            sdp: serde_json::to_string(&offer).map_err(|e| e.to_string())?,
                        }).await.map_err(|_| "signaling writer stopped".to_string())?;
                    }
                    SignalMessage::Offer { sdp } if as_guest => {
                        if sdp.len() > 48 * 1024 { return Err("oversized SDP offer".into()); }
                        let offer = serde_json::from_str(&sdp).map_err(|e| format!("invalid SDP offer: {e}"))?;
                        pc.set_remote_description(offer).await.map_err(|e| format!("apply SDP offer: {e}"))?;
                        remote_description_set = true;
                        for candidate in queued_candidates.drain(..) {
                            pc.add_ice_candidate(candidate).await.map_err(|e| e.to_string())?;
                        }
                        let answer = pc.create_answer(None).await.map_err(|e| e.to_string())?;
                        pc.set_local_description(answer.clone()).await.map_err(|e| e.to_string())?;
                        writer_tx.send(SignalMessage::Answer {
                            sdp: serde_json::to_string(&answer).map_err(|e| e.to_string())?,
                        }).await.map_err(|_| "signaling writer stopped".to_string())?;
                    }
                    SignalMessage::Answer { sdp } if !as_guest => {
                        if sdp.len() > 48 * 1024 { return Err("oversized SDP answer".into()); }
                        let answer = serde_json::from_str(&sdp).map_err(|e| format!("invalid SDP answer: {e}"))?;
                        pc.set_remote_description(answer).await.map_err(|e| format!("apply SDP answer: {e}"))?;
                        remote_description_set = true;
                        for candidate in queued_candidates.drain(..) {
                            pc.add_ice_candidate(candidate).await.map_err(|e| e.to_string())?;
                        }
                    }
                    SignalMessage::Candidate { candidate } => {
                        let candidate: RTCIceCandidateInit = serde_json::from_value(candidate)
                            .map_err(|e| format!("invalid ICE candidate: {e}"))?;
                        if remote_description_set {
                            pc.add_ice_candidate(candidate).await.map_err(|e| format!("add ICE candidate: {e}"))?;
                        } else if queued_candidates.len() < 128 {
                            queued_candidates.push(candidate);
                        } else {
                            return Err("too many ICE candidates".into());
                        }
                    }
                    SignalMessage::PeerLeft { .. } => return Err("peer left rendezvous room".into()),
                    SignalMessage::Error { message } => return Err(format!("rendezvous: {message}")),
                    _ => {}
                }
            }
            channel = channel_rx.recv(), if !channel_started => {
                let Some(channel) = channel else { continue; };
                data_channel = Some(channel.clone());
                channel_started = true;
                let runtime = runtime.clone();
                let connected = connected_tx.take();
                last_channel_task = Some(tokio::spawn(async move {
                    data_channel_loop(runtime, channel, connected).await;
                }));
            }
            result = &mut connected_rx, if connected_tx.is_none() => {
                if result.is_err() { return Err("peer data channel closed during handshake".into()); }
                break;
            }
        }
    }

    // Keep the signaling link alive with the data channel so trickle ICE and
    // room disconnect notifications remain available for ICE restarts.
    if let Some(task) = last_channel_task {
        let _ = task.await;
    }
    let _ = pc.close().await;
    reader_task.abort();
    writer_task.abort();
    let _ = data_channel;
    Ok(())
}

fn configured_ice_servers() -> Result<Vec<RTCIceServer>, String> {
    let stun = std::env::var("FORGE_STUN_SERVERS")
        .unwrap_or_else(|_| "stun:stun.l.google.com:19302".into());
    let mut servers: Vec<RTCIceServer> = stun
        .split(',')
        .map(str::trim)
        .filter(|url| !url.is_empty())
        .map(|url| RTCIceServer {
            urls: vec![url.to_string()],
            ..Default::default()
        })
        .collect();
    if let Ok(turn) = std::env::var("FORGE_TURN_URL") {
        let username = std::env::var("FORGE_TURN_USERNAME").unwrap_or_default();
        let credential = std::env::var("FORGE_TURN_CREDENTIAL").unwrap_or_default();
        if username.is_empty() || credential.is_empty() {
            return Err(
                "FORGE_TURN_URL requires FORGE_TURN_USERNAME and FORGE_TURN_CREDENTIAL".into(),
            );
        }
        servers.push(RTCIceServer {
            urls: turn
                .split(',')
                .map(str::trim)
                .filter(|url| !url.is_empty())
                .map(str::to_string)
                .collect(),
            username,
            credential,
        });
    }
    Ok(servers)
}

async fn data_channel_loop(
    runtime: PeerRuntime,
    channel: Arc<dyn DataChannel>,
    connected: Option<oneshot::Sender<()>>,
) {
    let (ephemeral_secret, ephemeral_public) = crate::crypto::Identity::new_ephemeral();
    let hello = runtime.local_hello(ephemeral_public).await;
    let hello_text = match serde_json::to_string(&PeerWireFrame::Hello(hello)) {
        Ok(text) => text,
        Err(_) => return,
    };
    let mut sent_hello = false;
    let mut handshaken = false;
    let mut confirmed = false;
    let mut remote_handle: Option<String> = None;
    let mut connected = connected;
    let mut pending_encrypted: Option<EncryptedFrame> = None;
    let mut chunk_assembly: Option<ChunkAssembly> = None;
    let mut hello_retry = tokio::time::interval_at(
        tokio::time::Instant::now() + Duration::from_secs(2),
        Duration::from_secs(2),
    );
    let handshake_deadline = tokio::time::Instant::now() + Duration::from_secs(120);

    loop {
        let event = tokio::select! {
            event = channel.poll() => event,
            _ = hello_retry.tick(), if !confirmed && sent_hello && tokio::time::Instant::now() < handshake_deadline => {
                if channel.send_text(&hello_text).await.is_err() { break; }
                continue;
            }
            _ = tokio::time::sleep_until(handshake_deadline), if !confirmed => break,
        };
        let Some(event) = event else {
            break;
        };
        match event {
            DataChannelEvent::OnOpen => {
                if !sent_hello {
                    if channel.send_text(&hello_text).await.is_err() {
                        break;
                    }
                    sent_hello = true;
                }
            }
            DataChannelEvent::OnMessage(message) => {
                if message.data.len() > MAX_DATA_CHANNEL_FRAME {
                    break;
                }
                let frame: PeerWireFrame = match serde_json::from_slice(&message.data) {
                    Ok(frame) => frame,
                    Err(_) => continue,
                };
                if let PeerWireFrame::Hello(remote) = frame {
                    if handshaken {
                        continue;
                    }
                    if message.data.len() > MAX_REMOTE_HELLO {
                        break;
                    }
                    if let Err(error) = runtime.verify_remote(&remote, None).await {
                        if error.contains("awaiting fingerprint approval") {
                            continue;
                        }
                        break;
                    }
                    let crypto = match runtime.identity.session_with_ephemeral(
                        &remote.public_key_b64,
                        &ephemeral_secret,
                        &remote.ephemeral_key_b64,
                    ) {
                        Ok(session) => session,
                        Err(_) => break,
                    };
                    remote_handle = Some(remote.handle.clone());
                    runtime.rtc_peers.lock().await.insert(
                        remote.handle.clone(),
                        Arc::new(RtcPeer {
                            channel: channel.clone(),
                            session: Mutex::new(crypto),
                        }),
                    );
                    let (local_handle, guild) = {
                        let state = runtime.guild_state.lock().await;
                        (state.my_handle.clone(), state.guild_name.clone())
                    };
                    runtime.guild_state.lock().await.add_or_update_peer(
                        remote.handle.clone(),
                        remote.guild.clone(),
                        String::new(),
                        String::new(),
                    );
                    let _ = runtime.events.send(crate::ipc::OutgoingMsg::PeerOnline {
                        handle: remote.handle.clone(),
                        file: String::new(),
                    });
                    handshaken = true;
                    // An encrypted presence frame confirms that this side also
                    // accepted the remote fingerprint. The other side may hold
                    // it until the user accepts its pending trust prompt.
                    if let Some(peer) = runtime.rtc_peers.lock().await.get(&remote.handle).cloned()
                    {
                        let _ = peer
                            .send(&PeerMsg::Presence {
                                from: local_handle,
                                guild,
                                file: String::new(),
                            })
                            .await;
                    }
                    if let Some(frame) = pending_encrypted.take() {
                        confirmed = process_encrypted(
                            &runtime,
                            remote_handle.as_deref(),
                            frame,
                            &mut chunk_assembly,
                        )
                        .await;
                    }
                    if let Some(connected) = connected.take() {
                        let _ = connected.send(());
                    }
                } else if let PeerWireFrame::Encrypted(encrypted) = frame {
                    if handshaken {
                        confirmed |= process_encrypted(
                            &runtime,
                            remote_handle.as_deref(),
                            encrypted,
                            &mut chunk_assembly,
                        )
                        .await;
                    } else {
                        pending_encrypted = Some(encrypted);
                    }
                } else if let PeerWireFrame::EncryptedChunk {
                    message_id,
                    index,
                    total,
                    frame,
                } = frame
                {
                    if handshaken {
                        confirmed |= process_chunk(
                            &runtime,
                            remote_handle.as_deref(),
                            message_id,
                            index,
                            total,
                            frame,
                            &mut chunk_assembly,
                        )
                        .await;
                    }
                }
            }
            DataChannelEvent::OnClose | DataChannelEvent::OnClosing => break,
            DataChannelEvent::OnError => break,
            _ => {}
        }
    }
    if let Some(handle) = remote_handle {
        let mut peers = runtime.rtc_peers.lock().await;
        if peers
            .get(&handle)
            .is_some_and(|peer| Arc::ptr_eq(&peer.channel, &channel))
        {
            peers.remove(&handle);
        }
        let _ = runtime
            .events
            .send(crate::ipc::OutgoingMsg::PeerOffline { handle });
    }
}

async fn process_encrypted(
    runtime: &PeerRuntime,
    handle: Option<&str>,
    frame: EncryptedFrame,
    assembly: &mut Option<ChunkAssembly>,
) -> bool {
    let Some(handle) = handle else {
        return false;
    };
    let peer = runtime.rtc_peers.lock().await.get(handle).cloned();
    let Some(peer) = peer else {
        return false;
    };
    let body = match peer.session.lock().await.decrypt(&frame) {
        Ok(body) => body,
        Err(_) => return false,
    };
    *assembly = None;
    dispatch_peer_message(runtime, body).await
}

async fn process_chunk(
    runtime: &PeerRuntime,
    handle: Option<&str>,
    message_id: u64,
    index: u16,
    total: u16,
    frame: EncryptedFrame,
    assembly: &mut Option<ChunkAssembly>,
) -> bool {
    if total == 0 || total as usize > RTC_MAX_CHUNKS || index >= total {
        *assembly = None;
        return false;
    }
    let Some(handle) = handle else {
        return false;
    };
    let peer = runtime.rtc_peers.lock().await.get(handle).cloned();
    let Some(peer) = peer else {
        return false;
    };
    let chunk = match peer.session.lock().await.decrypt(&frame) {
        Ok(chunk) if chunk.len() <= RTC_CHUNK_BYTES => chunk,
        _ => {
            *assembly = None;
            return false;
        }
    };
    let body = match accept_chunk(assembly, message_id, index, total, chunk) {
        Ok(body) => body,
        Err(()) => return false,
    };
    let Some(body) = body else {
        return false;
    };
    dispatch_peer_message(runtime, body).await
}

fn accept_chunk(
    assembly: &mut Option<ChunkAssembly>,
    message_id: u64,
    index: u16,
    total: u16,
    chunk: Vec<u8>,
) -> Result<Option<Vec<u8>>, ()> {
    if total == 0
        || total as usize > RTC_MAX_CHUNKS
        || index >= total
        || chunk.len() > RTC_CHUNK_BYTES
    {
        *assembly = None;
        return Err(());
    }
    if index == 0 {
        *assembly = Some(ChunkAssembly {
            message_id,
            total,
            next_index: 0,
            bytes: Vec::new(),
        });
    }
    let Some(current) = assembly.as_mut() else {
        return Err(());
    };
    if current.message_id != message_id
        || current.total != total
        || current.next_index != index
        || current.bytes.len().saturating_add(chunk.len()) > RTC_MAX_PEER_MESSAGE
    {
        *assembly = None;
        return Err(());
    }
    current.bytes.extend_from_slice(&chunk);
    current.next_index += 1;
    if current.next_index != current.total {
        return Ok(None);
    }
    let complete = assembly
        .take()
        .map(|message| message.bytes)
        .unwrap_or_default();
    Ok(Some(complete))
}

async fn dispatch_peer_message(runtime: &PeerRuntime, body: Vec<u8>) -> bool {
    let Ok(message) = serde_json::from_slice::<PeerMsg>(&body) else {
        return false;
    };
    crate::peer::handle_peer_msg(message, runtime).await;
    true
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn fragmented_payload_reassembles_in_order_and_rejects_gaps() {
        let mut assembly = None;
        assert_eq!(
            accept_chunk(&mut assembly, 3, 0, 2, b"first".to_vec()).unwrap(),
            None
        );
        assert_eq!(
            accept_chunk(&mut assembly, 3, 1, 2, b"second".to_vec()).unwrap(),
            Some(b"firstsecond".to_vec())
        );
        assert!(accept_chunk(&mut assembly, 4, 1, 2, b"gap".to_vec()).is_err());
        assert!(assembly.is_none());
    }
}
