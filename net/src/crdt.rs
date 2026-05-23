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
    },
    #[serde(rename = "delete")]
    Delete {
        id: CharId,
    },
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

        for ch in text.chars() {
            let id = doc.next_id();
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
        let parent = if pos == 0 {
            None
        } else {
            // Find the visible character at position pos-1
            let mut visible_count = 0;
            let mut parent_id = None;
            for c in &self.chars {
                if !c.deleted {
                    visible_count += 1;
                    if visible_count == pos {
                        parent_id = Some(c.id);
                        break;
                    }
                }
            }
            parent_id
        };

        let id = self.next_id();
        let op = CrdtOp::Insert {
            id,
            value,
            parent,
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
            CrdtOp::Insert { id, value, parent } => {
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
                let insert_idx = if let Some(parent_id) = parent {
                    if let Some(&parent_idx) = self.id_index.get(parent_id) {
                        // Insert after parent, but before any existing children with
                        // higher agent IDs (for deterministic ordering)
                        let mut idx = parent_idx + 1;
                        while idx < self.chars.len() {
                            if let Some(ref their_parent) = self.chars[idx].parent {
                                if *their_parent == *parent_id {
                                    // Same parent — compare agent IDs for ordering
                                    if self.chars[idx].id.agent > id.agent {
                                        break;
                                    }
                                    if self.chars[idx].id.agent == id.agent
                                        && self.chars[idx].id.seq > id.seq
                                    {
                                        break;
                                    }
                                }
                            }
                            idx += 1;
                        }
                        idx
                    } else {
                        self.chars.len()
                    }
                } else {
                    // No parent — insert at beginning, but after any existing
                    // beginning chars with higher priority
                    let mut idx = 0;
                    while idx < self.chars.len() {
                        if self.chars[idx].parent.is_none() {
                            if self.chars[idx].id.agent > id.agent {
                                break;
                            }
                        } else {
                            break;
                        }
                        idx += 1;
                    }
                    idx
                };

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
