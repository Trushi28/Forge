//! forge-net library — public API for integration tests
//!
//! The main binary lives in main.rs; this lib.rs re-exports
//! modules so that integration tests can import them.

pub mod collab;
pub mod crdt;
pub mod crypto;
pub mod discovery;
pub mod guild;
pub mod ipc;
pub mod peer;
pub mod transfer;
