use std::time::SystemTime;

/// Represents a peer on the network
#[derive(Debug, Clone)]
pub struct GuildPeer {
    pub handle: String,
    pub name: String,
    pub color: String,
    pub addr: String,
    pub last_seen: SystemTime,
    pub current_file: String,
}

/// Chat message
#[derive(Debug, Clone)]
pub struct ChatMessage {
    pub from: String,
    pub text: String,
    pub timestamp: SystemTime,
    pub is_dm: bool,
}

/// Ping notification
#[derive(Debug, Clone)]
pub struct PingNotification {
    pub from: String,
    pub file: String,
    pub line: i32,
    pub context: String,
    pub timestamp: SystemTime,
}

/// A shared file entry
#[derive(Debug, Clone)]
pub struct SharedFileEntry {
    pub name: String,
    pub from: String,
    pub size: usize,
    pub timestamp: SystemTime,
}

/// Guild state: peers, chat, pings, shared files, and configuration
pub struct GuildState {
    pub guild_name: String,
    pub my_handle: String,
    pub my_color: String,
    pub peers: Vec<GuildPeer>,
    pub chat_history: Vec<ChatMessage>,
    pub pending_pings: Vec<PingNotification>,
    pub shared_files: Vec<SharedFileEntry>,
    pub port: u16,
}

impl GuildState {
    pub fn new() -> Self {
        GuildState {
            guild_name: String::new(),
            my_handle: "anon".to_string(),
            my_color: "cyan".to_string(),
            peers: Vec::new(),
            chat_history: Vec::new(),
            pending_pings: Vec::new(),
            shared_files: Vec::new(),
            port: 9876,
        }
    }

    /// Load configuration from profile.toml
    pub fn load_config(&mut self) {
        if let Ok(home) = std::env::var("HOME") {
            let path = format!("{}/.config/forge/profile.toml", home);
            if let Ok(content) = std::fs::read_to_string(&path) {
                for line in content.lines() {
                    let line = line.trim();
                    if line.starts_with('#') || line.is_empty() {
                        continue;
                    }
                    if let Some((key, val)) = line.split_once('=') {
                        let key = key.trim();
                        let val = val.trim().trim_matches('"');
                        match key {
                            "handle" => self.my_handle = val.to_string(),
                            "guild" | "name" => self.guild_name = val.to_string(),
                            "color" => self.my_color = val.to_string(),
                            _ => {}
                        }
                    }
                }
                eprintln!(
                    "forge-net: loaded profile: {} in guild '{}'",
                    self.my_handle, self.guild_name
                );
            }
        }
    }

    pub fn add_peer(&mut self, peer: GuildPeer) {
        // Update if existing, add if new
        if let Some(existing) = self.peers.iter_mut().find(|p| p.handle == peer.handle) {
            existing.addr = peer.addr;
            existing.last_seen = peer.last_seen;
            existing.current_file = peer.current_file;
        } else {
            self.peers.push(peer);
        }
    }

    pub fn add_or_update_peer(
        &mut self,
        handle: String,
        guild_name: String,
        addr: String,
        current_file: String,
    ) {
        let peer = GuildPeer {
            handle,
            name: guild_name,
            color: "cyan".to_string(),
            addr,
            last_seen: SystemTime::now(),
            current_file,
        };
        self.add_peer(peer);
    }

    pub fn remove_peer(&mut self, handle: &str) {
        self.peers.retain(|p| p.handle != handle);
    }

    pub fn find_peer(&self, handle: &str) -> Option<&GuildPeer> {
        self.peers.iter().find(|p| p.handle == handle)
    }

    pub fn add_chat_message(&mut self, from: String, text: String) {
        self.chat_history.push(ChatMessage {
            from,
            text,
            timestamp: SystemTime::now(),
            is_dm: false,
        });

        // Keep only last 1000 messages
        if self.chat_history.len() > 1000 {
            self.chat_history.drain(0..self.chat_history.len() - 1000);
        }
    }

    pub fn add_dm(&mut self, from: String, text: String) {
        self.chat_history.push(ChatMessage {
            from,
            text,
            timestamp: SystemTime::now(),
            is_dm: true,
        });
    }

    pub fn add_ping(&mut self, from: String, file: String, line: i32, context: String) {
        self.pending_pings.push(PingNotification {
            from,
            file,
            line,
            context,
            timestamp: SystemTime::now(),
        });
    }

    pub fn pop_ping(&mut self) -> Option<PingNotification> {
        if self.pending_pings.is_empty() {
            None
        } else {
            Some(self.pending_pings.remove(0))
        }
    }

    pub fn add_shared_file(&mut self, name: String, from: String, size: usize) {
        // Remove existing with same name from same user
        self.shared_files
            .retain(|f| !(f.name == name && f.from == from));

        self.shared_files.push(SharedFileEntry {
            name,
            from,
            size,
            timestamp: SystemTime::now(),
        });
    }

    pub fn remove_shared_file(&mut self, name: &str, from: &str) {
        self.shared_files
            .retain(|f| !(f.name == name && f.from == from));
    }

    pub fn peer_count(&self) -> usize {
        self.peers.len()
    }

    pub fn online_peers(&self) -> Vec<&GuildPeer> {
        self.peers.iter().collect()
    }
}
