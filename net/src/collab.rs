//! Collab session management — initiate, accept, and manage
//! collaborative editing sessions between peers.

use crate::crdt::{CollabDoc, CrdtOp};
use crate::guild::GuildPeer;
use serde::{Deserialize, Serialize};
use std::collections::HashMap;

/// State of a collab session
#[derive(Debug, Clone, PartialEq)]
pub enum SessionState {
    Pending, // Invitation sent, waiting for response
    Active,  // Both peers are actively editing
    Closed,  // Session has ended
}

/// A collab session between two peers editing the same file
pub struct CollabSession {
    pub id: u64,
    pub file: String,
    pub peer_handle: String,
    pub peer_addr: String,
    pub state: SessionState,
    pub agent_id: u32,
    doc: CollabDoc,
    /// Pending ops to send to peer
    outgoing_ops: Vec<CrdtOp>,
}

impl CollabSession {
    /// Create a new session as the initiator
    pub fn new_as_initiator(id: u64, file: &str, peer: &GuildPeer, initial_text: &str) -> Self {
        CollabSession {
            id,
            file: file.to_string(),
            peer_handle: peer.handle.clone(),
            peer_addr: peer.addr.clone(),
            state: SessionState::Pending,
            agent_id: 1, // initiator is always agent 1
            doc: CollabDoc::from_text(1, initial_text),
            outgoing_ops: Vec::new(),
        }
    }

    /// Create a new session as the responder
    pub fn new_as_responder(id: u64, file: &str, peer: &GuildPeer, initial_text: &str) -> Self {
        CollabSession {
            id,
            file: file.to_string(),
            peer_handle: peer.handle.clone(),
            peer_addr: peer.addr.clone(),
            state: SessionState::Active,
            agent_id: 2, // responder is always agent 2
            doc: CollabDoc::from_text(2, initial_text),
            outgoing_ops: Vec::new(),
        }
    }

    /// Accept the session (transition from Pending to Active)
    pub fn accept(&mut self) {
        self.state = SessionState::Active;
    }

    /// Decline/close the session
    pub fn close(&mut self) {
        self.state = SessionState::Closed;
    }

    /// Apply a local edit and queue the operation for sending
    pub fn local_insert(&mut self, pos: usize, ch: char) -> CrdtOp {
        let op = self.doc.insert(pos, ch);
        self.outgoing_ops.push(op.clone());
        op
    }

    /// Apply a local delete and queue the operation for sending
    pub fn local_delete(&mut self, pos: usize) -> Option<CrdtOp> {
        if let Some(op) = self.doc.delete(pos) {
            self.outgoing_ops.push(op.clone());
            Some(op)
        } else {
            None
        }
    }

    /// Apply a remote operation from the peer
    pub fn apply_remote(&mut self, op: &CrdtOp) {
        self.doc.apply_op(op);
    }

    /// Get the current document text
    pub fn text(&self) -> String {
        self.doc.text()
    }

    /// Drain outgoing operations (returns them and clears the queue)
    pub fn drain_outgoing(&mut self) -> Vec<CrdtOp> {
        std::mem::take(&mut self.outgoing_ops)
    }

    /// Check if session is active
    pub fn is_active(&self) -> bool {
        self.state == SessionState::Active
    }
}

/// Manages all active collab sessions
pub struct CollabManager {
    sessions: HashMap<u64, CollabSession>,
    next_id: u64,
}

impl CollabManager {
    pub fn new() -> Self {
        CollabManager {
            sessions: HashMap::new(),
            next_id: 1,
        }
    }

    /// Create a new session (as initiator)
    pub fn create_session(&mut self, file: &str, peer: &GuildPeer, initial_text: &str) -> u64 {
        let id = self.next_id;
        self.next_id += 1;
        let session = CollabSession::new_as_initiator(id, file, peer, initial_text);
        self.sessions.insert(id, session);
        id
    }

    /// Accept an incoming session request
    pub fn accept_session(
        &mut self,
        id: u64,
        file: &str,
        peer: &GuildPeer,
        initial_text: &str,
    ) -> u64 {
        let session = CollabSession::new_as_responder(id, file, peer, initial_text);
        self.sessions.insert(id, session);
        id
    }

    /// Get a mutable reference to a session
    pub fn get_session(&mut self, id: u64) -> Option<&mut CollabSession> {
        self.sessions.get_mut(&id)
    }

    /// Close a session
    pub fn close_session(&mut self, id: u64) {
        if let Some(session) = self.sessions.get_mut(&id) {
            session.close();
        }
    }

    /// Remove all closed sessions
    pub fn cleanup(&mut self) {
        self.sessions.retain(|_, s| s.state != SessionState::Closed);
    }

    /// Get all active session IDs
    pub fn active_sessions(&self) -> Vec<u64> {
        self.sessions
            .iter()
            .filter(|(_, s)| s.is_active())
            .map(|(id, _)| *id)
            .collect()
    }

    /// Find session by file and peer
    pub fn find_session(&self, file: &str, peer_handle: &str) -> Option<u64> {
        self.sessions
            .iter()
            .find(|(_, s)| s.file == file && s.peer_handle == peer_handle && s.is_active())
            .map(|(id, _)| *id)
    }
}

/// Messages for collab protocol (sent between peers)
#[derive(Debug, Serialize, Deserialize)]
#[serde(tag = "type")]
pub enum CollabMsg {
    #[serde(rename = "COLLAB_REQUEST")]
    Request {
        session_id: u64,
        file: String,
        from: String,
        initial_text: String,
    },
    #[serde(rename = "COLLAB_ACCEPT")]
    Accept { session_id: u64, from: String },
    #[serde(rename = "COLLAB_DECLINE")]
    Decline { session_id: u64, from: String },
    #[serde(rename = "COLLAB_OP")]
    Operation { session_id: u64, op: CrdtOp },
    #[serde(rename = "COLLAB_CLOSE")]
    Close { session_id: u64, from: String },
    #[serde(rename = "COLLAB_CURSOR")]
    Cursor {
        session_id: u64,
        from: String,
        line: i32,
        col: i32,
    },
}
