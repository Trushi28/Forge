//! P2P file transfer — direct TCP, no cloud
//!
//! Handles file sharing between Forge peers. Files are sent directly
//! peer-to-peer over TCP. A shared file pool tracks available files.

use serde::{Deserialize, Serialize};
use std::collections::HashMap;
use std::path::Path;
use std::sync::Arc;
use std::time::SystemTime;
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::{TcpListener, TcpStream};
use tokio::sync::Mutex;

/// A file available in the shared pool
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct SharedFile {
    pub name: String,
    pub from: String,
    pub size: usize,
    pub timestamp: u64,
    pub data: Vec<u8>,
}

/// Shared file pool — tracks files shared by all peers
pub struct FilePool {
    pub files: HashMap<String, SharedFile>,
}

impl FilePool {
    pub fn new() -> Self {
        FilePool {
            files: HashMap::new(),
        }
    }

    pub fn add_file(&mut self, file: SharedFile) {
        let key = format!("{}:{}", file.from, file.name);
        self.files.insert(key, file);
    }

    pub fn remove_file(&mut self, from: &str, name: &str) {
        let key = format!("{}:{}", from, name);
        self.files.remove(&key);
    }

    pub fn list_files(&self) -> Vec<&SharedFile> {
        self.files.values().collect()
    }
}

/// Message format for file transfer protocol
#[derive(Debug, Serialize, Deserialize)]
#[serde(tag = "type")]
enum TransferMsg {
    #[serde(rename = "FILE_OFFER")]
    FileOffer {
        name: String,
        size: usize,
        from: String,
    },
    #[serde(rename = "FILE_ACCEPT")]
    FileAccept,
    #[serde(rename = "FILE_DATA")]
    FileData { name: String, data_b64: String },
}

/// Send a file to a peer at the given address
pub async fn send_file(
    peer_addr: &str,
    path: &Path,
    from_handle: &str,
) -> Result<(), Box<dyn std::error::Error>> {
    let data = tokio::fs::read(path).await?;
    let name = path
        .file_name()
        .and_then(|n| n.to_str())
        .unwrap_or("unknown")
        .to_string();

    let mut stream = TcpStream::connect(peer_addr).await?;

    // Send file offer
    let offer = TransferMsg::FileOffer {
        name: name.clone(),
        size: data.len(),
        from: from_handle.to_string(),
    };
    let offer_json = serde_json::to_string(&offer)?;
    let len = offer_json.len() as u32;
    stream.write_all(&len.to_be_bytes()).await?;
    stream.write_all(offer_json.as_bytes()).await?;

    // Send file data as base64
    let data_msg = TransferMsg::FileData {
        name,
        data_b64: base64_encode(&data),
    };
    let data_json = serde_json::to_string(&data_msg)?;
    let len = data_json.len() as u32;
    stream.write_all(&len.to_be_bytes()).await?;
    stream.write_all(data_json.as_bytes()).await?;

    eprintln!("forge-net: file sent to {}", peer_addr);
    Ok(())
}

/// Listen for incoming file transfers
pub async fn listen_for_transfers(
    port: u16,
    file_pool: Arc<Mutex<FilePool>>,
) -> Result<(), Box<dyn std::error::Error>> {
    let listener = TcpListener::bind(format!("0.0.0.0:{}", port)).await?;
    eprintln!("forge-net: file transfer listener on port {}", port);

    loop {
        let (mut stream, addr) = listener.accept().await?;
        let pool = file_pool.clone();

        tokio::spawn(async move {
            eprintln!("forge-net: incoming transfer from {}", addr);

            // Read file offer
            let mut len_buf = [0u8; 4];
            if stream.read_exact(&mut len_buf).await.is_err() {
                return;
            }
            let len = u32::from_be_bytes(len_buf) as usize;
            if len > 10 * 1024 * 1024 {
                return; // Too large
            }
            let mut buf = vec![0u8; len];
            if stream.read_exact(&mut buf).await.is_err() {
                return;
            }

            // Read file data
            if stream.read_exact(&mut len_buf).await.is_err() {
                return;
            }
            let data_len = u32::from_be_bytes(len_buf) as usize;
            if data_len > 50 * 1024 * 1024 {
                return; // Too large
            }
            let mut data_buf = vec![0u8; data_len];
            if stream.read_exact(&mut data_buf).await.is_err() {
                return;
            }

            if let Ok(msg) = serde_json::from_slice::<TransferMsg>(&data_buf) {
                if let TransferMsg::FileData { name, data_b64 } = msg {
                    let data = base64_decode(&data_b64);
                    let file = SharedFile {
                        name: name.clone(),
                        from: addr.to_string(),
                        size: data.len(),
                        timestamp: SystemTime::now()
                            .duration_since(SystemTime::UNIX_EPOCH)
                            .map(|d| d.as_secs())
                            .unwrap_or(0),
                        data,
                    };

                    let mut pool = pool.lock().await;
                    pool.add_file(file);
                    eprintln!("forge-net: received file: {}", name);
                }
            }
        });
    }
}

/// Simple base64 encoding (no external dependency)
fn base64_encode(data: &[u8]) -> String {
    use base64::Engine;
    base64::engine::general_purpose::STANDARD.encode(data)
}

/// Simple base64 decoding
fn base64_decode(input: &str) -> Vec<u8> {
    use base64::Engine;
    base64::engine::general_purpose::STANDARD
        .decode(input)
        .unwrap_or_default()
}

/// Public wrapper for base64 decode (used by ipc.rs)
pub fn base64_decode_pub(input: &str) -> Vec<u8> {
    base64_decode(input)
}

/// Public wrapper for base64 encode (used by ipc.rs)
pub fn base64_encode_pub(data: &[u8]) -> String {
    base64_encode(data)
}
