//! Integration tests for the forge-net layer.
//!
//! These test the key subsystems together:
//!   - CRDT convergence across multiple peers
//!   - Collab session lifecycle
//!   - IPC message serialization/deserialization
//!   - Guild state management

use forge_net::crdt::CollabDoc;
use forge_net::collab::{CollabManager, SessionState};
use forge_net::guild::{GuildPeer, GuildState};
use forge_net::ipc::{IncomingMsg, OutgoingMsg};
use std::time::SystemTime;

// ═══════════════════════════════════════════════════════════════
// CRDT convergence tests
// ═══════════════════════════════════════════════════════════════

#[test]
fn test_crdt_two_peer_convergence() {
    // Simulate two peers making interleaved edits
    let mut peer1 = CollabDoc::from_text(1, "Hello");
    let mut peer2 = CollabDoc::from_text(2, "Hello");

    // Peer1 inserts " World" at end
    let ops1: Vec<_> = " World".chars().enumerate().map(|(i, c)| {
        peer1.insert(5 + i, c)
    }).collect();

    // Peer2 inserts "!" at end (before receiving peer1's ops)
    let op2 = peer2.insert(5, '!');

    // Exchange ops
    for op in &ops1 {
        peer2.apply_op(op);
    }
    peer1.apply_op(&op2);

    // Both should converge
    assert_eq!(peer1.text(), peer2.text());
    // The text should contain all characters
    let text = peer1.text();
    assert!(text.contains("Hello"), "should contain Hello");
    assert!(text.contains("World"), "should contain World");
    assert!(text.contains("!"), "should contain !");
}

#[test]
fn test_crdt_three_peer_convergence() {
    // Three peers all start from same text
    let mut p1 = CollabDoc::from_text(1, "abc");
    let mut p2 = CollabDoc::from_text(2, "abc");
    let mut p3 = CollabDoc::from_text(3, "abc");

    // Each peer inserts at position 1
    let op1 = p1.insert(1, 'X');
    let op2 = p2.insert(1, 'Y');
    let op3 = p3.insert(1, 'Z');

    // Exchange all ops
    for op in [&op1, &op2, &op3] {
        p1.apply_op(op);
        p2.apply_op(op);
        p3.apply_op(op);
    }

    // All three must converge
    assert_eq!(p1.text(), p2.text());
    assert_eq!(p2.text(), p3.text());
    // All 6 chars should be present
    assert_eq!(p1.len(), 6); // a + X + Y + Z + b + c
}

#[test]
fn test_crdt_delete_convergence() {
    let mut p1 = CollabDoc::from_text(1, "abcde");
    let mut p2 = CollabDoc::from_text(2, "abcde");

    // Peer1 deletes 'b', peer2 deletes 'd'
    let op1 = p1.delete(1).unwrap();
    let op2 = p2.delete(3).unwrap();

    p1.apply_op(&op2);
    p2.apply_op(&op1);

    assert_eq!(p1.text(), p2.text());
    assert_eq!(p1.text(), "ace");
}

#[test]
fn test_crdt_concurrent_insert_and_delete() {
    let mut p1 = CollabDoc::from_text(1, "abc");
    let mut p2 = CollabDoc::from_text(2, "abc");

    // Peer1 inserts 'X' at pos 1, peer2 deletes char at pos 1 ('b')
    let op1 = p1.insert(1, 'X');
    let op2 = p2.delete(1).unwrap();

    p1.apply_op(&op2);
    p2.apply_op(&op1);

    assert_eq!(p1.text(), p2.text());
    // 'X' was inserted, 'b' was deleted — "aXc"
    let text = p1.text();
    assert!(text.contains('X'));
    assert!(!text.contains('b'));
}

// ═══════════════════════════════════════════════════════════════
// Collab session lifecycle
// ═══════════════════════════════════════════════════════════════

fn make_peer(handle: &str) -> GuildPeer {
    GuildPeer {
        handle: handle.to_string(),
        name: "test-guild".to_string(),
        color: "cyan".to_string(),
        addr: "127.0.0.1:9876".to_string(),
        last_seen: SystemTime::now(),
        current_file: String::new(),
    }
}

#[test]
fn test_collab_session_create_accept_close() {
    let mut mgr = CollabManager::new();
    let peer = make_peer("alice");

    // Create session
    let id = mgr.create_session("main.c", &peer, "int main() {}");
    assert!(id > 0);

    // Session should be pending
    {
        let session = mgr.get_session(id).unwrap();
        assert_eq!(session.state, SessionState::Pending);
        assert_eq!(session.file, "main.c");
        assert_eq!(session.peer_handle, "alice");
    }

    // Accept
    {
        let session = mgr.get_session(id).unwrap();
        session.accept();
        assert_eq!(session.state, SessionState::Active);
    }

    // Active sessions should include this one
    let active = mgr.active_sessions();
    assert!(active.contains(&id));

    // Close
    mgr.close_session(id);
    {
        let session = mgr.get_session(id).unwrap();
        assert_eq!(session.state, SessionState::Closed);
    }

    // Cleanup should remove it
    mgr.cleanup();
    assert!(mgr.get_session(id).is_none());
}

#[test]
fn test_collab_local_edits_produce_ops() {
    let mut mgr = CollabManager::new();
    let peer = make_peer("bob");

    let id = mgr.create_session("test.rs", &peer, "fn main()");
    let session = mgr.get_session(id).unwrap();
    session.accept();

    // Local insert
    let op = session.local_insert(9, '{');
    assert_eq!(session.text(), "fn main(){");

    // Outgoing ops should be queued
    let outgoing = session.drain_outgoing();
    assert_eq!(outgoing.len(), 1);
    // After drain, should be empty
    assert!(session.drain_outgoing().is_empty());

    // Verify the op can be applied to another doc
    let mut remote = CollabDoc::from_text(2, "fn main()");
    remote.apply_op(&op);
    assert_eq!(remote.text(), "fn main(){");
}

#[test]
fn test_collab_find_session() {
    let mut mgr = CollabManager::new();
    let alice = make_peer("alice");
    let bob = make_peer("bob");

    let id1 = mgr.create_session("file1.c", &alice, "");
    let id2 = mgr.create_session("file2.c", &bob, "");

    mgr.get_session(id1).unwrap().accept();
    mgr.get_session(id2).unwrap().accept();

    assert_eq!(mgr.find_session("file1.c", "alice"), Some(id1));
    assert_eq!(mgr.find_session("file2.c", "bob"), Some(id2));
    assert_eq!(mgr.find_session("file3.c", "charlie"), None);
}

// ═══════════════════════════════════════════════════════════════
// IPC message serialization
// ═══════════════════════════════════════════════════════════════

#[test]
fn test_ipc_incoming_msg_deserialization() {
    // ChatSend
    let json = r#"{"type":"CHAT_SEND","guild":"test","text":"hello"}"#;
    let msg: IncomingMsg = serde_json::from_str(json).unwrap();
    match msg {
        IncomingMsg::ChatSend { guild, text } => {
            assert_eq!(guild, "test");
            assert_eq!(text, "hello");
        }
        _ => panic!("expected ChatSend"),
    }

    // CollabStart
    let json = r#"{"type":"COLLAB_START","peer":"alice","file":"main.c"}"#;
    let msg: IncomingMsg = serde_json::from_str(json).unwrap();
    match msg {
        IncomingMsg::CollabStart { peer, file } => {
            assert_eq!(peer, "alice");
            assert_eq!(file, "main.c");
        }
        _ => panic!("expected CollabStart"),
    }

    // Status
    let json = r#"{"type":"STATUS"}"#;
    let msg: IncomingMsg = serde_json::from_str(json).unwrap();
    assert!(matches!(msg, IncomingMsg::Status));
}

#[test]
fn test_ipc_outgoing_msg_serialization() {
    let msg = OutgoingMsg::ChatRecv {
        from: "alice".to_string(),
        text: "hi".to_string(),
    };
    let json = serde_json::to_string(&msg).unwrap();
    assert!(json.contains("\"type\":\"CHAT_RECV\""));
    assert!(json.contains("\"from\":\"alice\""));
    assert!(json.contains("\"text\":\"hi\""));

    // Verify it round-trips
    let parsed: serde_json::Value = serde_json::from_str(&json).unwrap();
    assert_eq!(parsed["type"], "CHAT_RECV");
}

#[test]
fn test_ipc_collab_op_format() {
    // The C editor sends COLLAB_OP with a JSON op_json field
    let json = r#"{"type":"COLLAB_OP","session_id":1,"op_json":"{\"pos\":5,\"is_insert\":true,\"text\":\"x\"}"}"#;
    let msg: IncomingMsg = serde_json::from_str(json).unwrap();
    match msg {
        IncomingMsg::CollabOp { session_id, op_json } => {
            assert_eq!(session_id, 1);
            assert!(op_json.contains("pos"));
        }
        _ => panic!("expected CollabOp"),
    }
}

// ═══════════════════════════════════════════════════════════════
// Guild state management
// ═══════════════════════════════════════════════════════════════

#[test]
fn test_guild_peer_management() {
    let mut gs = GuildState::new();
    assert_eq!(gs.peer_count(), 0);

    gs.add_or_update_peer(
        "alice".to_string(),
        "guild1".to_string(),
        "1.2.3.4:9876".to_string(),
        "main.c".to_string(),
    );
    assert_eq!(gs.peer_count(), 1);
    assert!(gs.find_peer("alice").is_some());
    assert_eq!(gs.find_peer("alice").unwrap().addr, "1.2.3.4:9876");

    // Update same peer
    gs.add_or_update_peer(
        "alice".to_string(),
        "guild1".to_string(),
        "1.2.3.4:9999".to_string(),
        "test.rs".to_string(),
    );
    assert_eq!(gs.peer_count(), 1); // should not duplicate
    assert_eq!(gs.find_peer("alice").unwrap().addr, "1.2.3.4:9999");

    // Add another peer
    gs.add_or_update_peer(
        "bob".to_string(),
        "guild1".to_string(),
        "5.6.7.8:9876".to_string(),
        String::new(),
    );
    assert_eq!(gs.peer_count(), 2);

    // Remove
    gs.remove_peer("alice");
    assert_eq!(gs.peer_count(), 1);
    assert!(gs.find_peer("alice").is_none());
}

#[test]
fn test_guild_chat_history() {
    let mut gs = GuildState::new();
    gs.add_chat_message("alice".to_string(), "hello".to_string());
    gs.add_chat_message("bob".to_string(), "hi there".to_string());

    assert_eq!(gs.chat_history.len(), 2);
    assert_eq!(gs.chat_history[0].from, "alice");
    assert_eq!(gs.chat_history[1].text, "hi there");

    // DMs
    gs.add_dm("charlie".to_string(), "secret msg".to_string());
    assert_eq!(gs.chat_history.len(), 3);
    assert!(gs.chat_history[2].is_dm);
}

#[test]
fn test_guild_shared_files() {
    let mut gs = GuildState::new();
    gs.add_shared_file("README.md".to_string(), "alice".to_string(), 1024);
    gs.add_shared_file("main.c".to_string(), "bob".to_string(), 2048);

    assert_eq!(gs.shared_files.len(), 2);

    // Update existing file from same user
    gs.add_shared_file("README.md".to_string(), "alice".to_string(), 4096);
    assert_eq!(gs.shared_files.len(), 2); // should replace, not add

    // Remove
    gs.remove_shared_file("main.c", "bob");
    assert_eq!(gs.shared_files.len(), 1);
}

#[test]
fn test_guild_pings() {
    let mut gs = GuildState::new();
    gs.add_ping("alice".to_string(), "main.c".to_string(), 42, String::new());
    gs.add_ping("bob".to_string(), "test.rs".to_string(), 10, String::new());

    assert_eq!(gs.pending_pings.len(), 2);

    let ping = gs.pop_ping().unwrap();
    assert_eq!(ping.from, "alice");
    assert_eq!(ping.line, 42);

    assert_eq!(gs.pending_pings.len(), 1);
}
