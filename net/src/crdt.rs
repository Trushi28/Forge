//! CRDT engine — conflict-free replicated data type for collaborative editing
//!
//! This provides a basic sequence CRDT for real-time collaborative text editing.
//! Each character has a unique ID (agent, seq), and operations can be applied
//! in any order while guaranteeing convergence.
//!
//! This is a simplified CRDT suitable for the MVP. For production use,
//! consider integrating diamond-types for better performance.

use serde::{Deserialize, Serialize};
use std::collections::HashMap;

/// Unique identifier for a CRDT character
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Serialize, Deserialize)]
pub struct CharId {
    pub agent: u32,
    pub seq: u32,
}

/// A character in the CRDT document
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct CrdtChar {
    pub id: CharId,
    pub value: char,
    pub deleted: bool,
    /// ID of the character this was inserted after (None = beginning)
    pub parent: Option<CharId>,
}

/// An operation that can be sent over the network
#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(tag = "op_type")]
pub enum CrdtOp {
    #[serde(rename = "insert")]
    Insert {
        id: CharId,
        value: char,
        parent: Option<CharId>,
        #[serde(default)]
        before: Option<CharId>,
    },
    #[serde(rename = "delete")]
    Delete { id: CharId },
}

/// A collaborative document backed by a sequence CRDT
pub struct CollabDoc {
    chars: Vec<CrdtChar>,
    agent: u32,
    seq_counter: u32,
    /// Map from CharId to index in chars array for fast lookup
    id_index: HashMap<CharId, usize>,
}

impl CollabDoc {
    /// Create a new empty document with the given agent ID
    pub fn new(agent: u32) -> Self {
        CollabDoc {
            chars: Vec::new(),
            agent,
            seq_counter: 0,
            id_index: HashMap::new(),
        }
    }

    /// Create a document initialized with text
    pub fn from_text(agent: u32, text: &str) -> Self {
        let mut doc = Self::new(agent);
        let mut parent: Option<CharId> = None;
        let mut initial_seq = 0;

        for ch in text.chars() {
            initial_seq += 1;
            let id = CharId {
                agent: 0,
                seq: initial_seq,
            };
            let crdt_char = CrdtChar {
                id,
                value: ch,
                deleted: false,
                parent,
            };
            let idx = doc.chars.len();
            doc.chars.push(crdt_char);
            doc.id_index.insert(id, idx);
            parent = Some(id);
        }

        doc
    }

    /// Get the next unique ID for this agent
    fn next_id(&mut self) -> CharId {
        self.seq_counter += 1;
        CharId {
            agent: self.agent,
            seq: self.seq_counter,
        }
    }

    /// Insert a character at a logical position. Returns the operation to broadcast.
    pub fn insert(&mut self, pos: usize, value: char) -> CrdtOp {
        let (parent, before) = self.neighbor_ids(pos);

        let id = self.next_id();
        let op = CrdtOp::Insert {
            id,
            value,
            parent,
            before,
        };

        self.apply_op(&op);
        op
    }

    /// Delete the character at a logical position. Returns the operation to broadcast.
    pub fn delete(&mut self, pos: usize) -> Option<CrdtOp> {
        let mut visible_count = 0;
        let mut target_id = None;

        for c in &self.chars {
            if !c.deleted {
                if visible_count == pos {
                    target_id = Some(c.id);
                    break;
                }
                visible_count += 1;
            }
        }

        target_id.map(|id| {
            let op = CrdtOp::Delete { id };
            self.apply_op(&op);
            op
        })
    }

    /// Apply a remote operation (or our own, idempotently)
    pub fn apply_op(&mut self, op: &CrdtOp) {
        match op {
            CrdtOp::Insert {
                id,
                value,
                parent,
                before,
            } => {
                // Check if already applied
                if self.id_index.contains_key(id) {
                    return;
                }

                let crdt_char = CrdtChar {
                    id: *id,
                    value: *value,
                    deleted: false,
                    parent: *parent,
                };

                // Find insertion position
                let insert_idx = self.find_insert_idx(id, parent, before);

                self.chars.insert(insert_idx, crdt_char);

                // Rebuild index (insertion invalidates all indices after insert_idx)
                self.rebuild_index();
            }
            CrdtOp::Delete { id } => {
                if let Some(&idx) = self.id_index.get(id) {
                    self.chars[idx].deleted = true;
                }
            }
        }
    }

    /// Rebuild the ID → index map
    fn rebuild_index(&mut self) {
        self.id_index.clear();
        for (i, c) in self.chars.iter().enumerate() {
            self.id_index.insert(c.id, i);
        }
    }

    fn neighbor_ids(&self, pos: usize) -> (Option<CharId>, Option<CharId>) {
        let mut visible_pos = 0;
        let mut parent = None;
        let mut before = None;

        for c in &self.chars {
            if c.deleted {
                continue;
            }

            if visible_pos == pos {
                before = Some(c.id);
                break;
            }

            parent = Some(c.id);
            visible_pos += 1;
        }

        (parent, before)
    }

    fn find_insert_idx(
        &self,
        id: &CharId,
        parent: &Option<CharId>,
        before: &Option<CharId>,
    ) -> usize {
        let lower_bound = parent
            .and_then(|parent_id| self.id_index.get(&parent_id).copied().map(|idx| idx + 1))
            .unwrap_or(0);
        let upper_bound = before
            .and_then(|before_id| self.id_index.get(&before_id).copied())
            .unwrap_or(self.chars.len());

        let mut idx = lower_bound.min(self.chars.len());
        let end = upper_bound.min(self.chars.len());
        while idx < end {
            let candidate = self.chars[idx].id;
            if candidate.agent > id.agent || (candidate.agent == id.agent && candidate.seq > id.seq)
            {
                break;
            }
            idx += 1;
        }
        idx
    }

    /// Get the current text content
    pub fn text(&self) -> String {
        self.chars
            .iter()
            .filter(|c| !c.deleted)
            .map(|c| c.value)
            .collect()
    }

    /// Get the visible character count
    pub fn len(&self) -> usize {
        self.chars.iter().filter(|c| !c.deleted).count()
    }

    /// Check if empty
    pub fn is_empty(&self) -> bool {
        self.len() == 0
    }

    /// Get the agent ID
    pub fn agent_id(&self) -> u32 {
        self.agent
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_basic_insert() {
        let mut doc = CollabDoc::new(1);
        doc.insert(0, 'H');
        doc.insert(1, 'i');
        assert_eq!(doc.text(), "Hi");
    }

    #[test]
    fn test_delete() {
        let mut doc = CollabDoc::from_text(1, "Hello");
        doc.delete(0);
        assert_eq!(doc.text(), "ello");
    }

    #[test]
    fn test_concurrent_insert() {
        // Two agents insert at the same position
        let mut doc1 = CollabDoc::from_text(1, "ac");
        let mut doc2 = CollabDoc::from_text(2, "ac");

        let op1 = doc1.insert(1, 'b'); // agent 1 inserts 'b' at pos 1
        let op2 = doc2.insert(1, 'x'); // agent 2 inserts 'x' at pos 1

        // Apply remote ops
        doc1.apply_op(&op2);
        doc2.apply_op(&op1);

        // Both should converge to the same text
        assert_eq!(doc1.text(), doc2.text());
    }
}
