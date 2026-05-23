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
}

/// Guild state: peers, chat, and configuration
pub struct GuildState {
    pub guild_name: String,
    pub my_handle: String,
    pub my_color: String,
    pub peers: Vec<GuildPeer>,
    pub chat_history: Vec<ChatMessage>,
}

impl GuildState {
    pub fn new() -> Self {
        GuildState {
            guild_name: String::new(),
            my_handle: "anon".to_string(),
            my_color: "cyan".to_string(),
            peers: Vec::new(),
            chat_history: Vec::new(),
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

    pub fn remove_peer(&mut self, handle: &str) {
        self.peers.retain(|p| p.handle != handle);
    }

    pub fn add_chat_message(&mut self, from: String, text: String) {
        self.chat_history.push(ChatMessage {
            from,
            text,
            timestamp: SystemTime::now(),
        });

        // Keep only last 1000 messages
        if self.chat_history.len() > 1000 {
            self.chat_history.drain(0..self.chat_history.len() - 1000);
        }
    }
}
