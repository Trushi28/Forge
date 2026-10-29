//! Persistent identity, TOFU contact pinning, and peer-frame encryption.

use base64::Engine;
use base64::engine::general_purpose::{STANDARD, URL_SAFE_NO_PAD};
use chacha20poly1305::aead::{Aead, KeyInit};
use chacha20poly1305::{Key, XChaCha20Poly1305, XNonce};
use hkdf::Hkdf;
use rand_core::{OsRng, RngCore};
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::collections::HashMap;
use std::fs;
use std::io;
use std::path::{Path, PathBuf};
use x25519_dalek::{PublicKey, StaticSecret};

const IDENTITY_FILE: &str = "identity.json";
const CONTACTS_FILE: &str = "contacts.json";

#[derive(Clone)]
pub struct Identity {
    secret: StaticSecret,
    public: PublicKey,
}

#[derive(Serialize, Deserialize)]
struct IdentityFile {
    secret_b64: String,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct Contact {
    pub handle: String,
    pub guild: String,
    pub public_key_b64: String,
    pub fingerprint: String,
    pub addr: Option<String>,
    #[serde(default)]
    pub relay: Option<String>,
}

#[derive(Default, Serialize, Deserialize)]
struct ContactFile {
    contacts: Vec<Contact>,
}

pub struct ContactBook {
    path: PathBuf,
    contacts: HashMap<String, Contact>,
    pending: HashMap<String, Contact>,
}

#[derive(Debug)]
pub enum TrustDecision {
    Trusted,
    PendingTrust { fingerprint: String },
    ChangedKey { expected: String, received: String },
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct Invite {
    pub handle: String,
    pub guild: String,
    pub public_key_b64: String,
    pub fingerprint: String,
    pub addr: Option<String>,
    pub relay: Option<String>,
    #[serde(default)]
    pub signal_url: Option<String>,
    #[serde(default)]
    pub signal_room: Option<String>,
}

pub struct CryptoSession {
    tx: XChaCha20Poly1305,
    rx: XChaCha20Poly1305,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct EncryptedFrame {
    pub nonce_b64: String,
    pub body_b64: String,
}

impl Identity {
    pub fn load_or_create(dir: &Path) -> io::Result<Self> {
        fs::create_dir_all(dir)?;
        let path = dir.join(IDENTITY_FILE);
        if let Ok(bytes) = fs::read(&path) {
            let parsed: IdentityFile = serde_json::from_slice(&bytes)
                .map_err(|e| io::Error::new(io::ErrorKind::InvalidData, e))?;
            let secret_bytes = STANDARD
                .decode(parsed.secret_b64)
                .map_err(|e| io::Error::new(io::ErrorKind::InvalidData, e))?;
            let secret_bytes: [u8; 32] = secret_bytes
                .try_into()
                .map_err(|_| io::Error::new(io::ErrorKind::InvalidData, "bad identity key"))?;
            let secret = StaticSecret::from(secret_bytes);
            let public = PublicKey::from(&secret);
            restrict_file_permissions(&path);
            return Ok(Self { secret, public });
        }

        let secret = StaticSecret::random_from_rng(OsRng);
        let public = PublicKey::from(&secret);
        let saved = IdentityFile {
            secret_b64: STANDARD.encode(secret.to_bytes()),
        };
        fs::write(
            &path,
            serde_json::to_vec_pretty(&saved)
                .map_err(|e| io::Error::new(io::ErrorKind::InvalidData, e))?,
        )?;
        restrict_file_permissions(&path);
        Ok(Self { secret, public })
    }

    pub fn public_key_b64(&self) -> String {
        STANDARD.encode(self.public.as_bytes())
    }

    pub fn fingerprint(&self) -> String {
        fingerprint(self.public.as_bytes())
    }

    pub fn session(&self, remote_public_b64: &str) -> Result<CryptoSession, String> {
        let remote_public = decode_public_key(remote_public_b64)?;
        let shared = self.secret.diffie_hellman(&remote_public);

        let local = self.public.as_bytes();
        let remote = remote_public.as_bytes();
        let mut salt = Vec::with_capacity(64);
        if local <= remote {
            salt.extend_from_slice(local);
            salt.extend_from_slice(remote);
        } else {
            salt.extend_from_slice(remote);
            salt.extend_from_slice(local);
        }

        let hk = Hkdf::<Sha256>::new(Some(&salt), shared.as_bytes());
        let mut okm = [0u8; 64];
        hk.expand(b"forge-net peer session v1", &mut okm)
            .map_err(|_| "key derivation failed".to_string())?;

        let local_is_low = local <= remote;
        let (tx_key, rx_key) = if local_is_low {
            (&okm[..32], &okm[32..])
        } else {
            (&okm[32..], &okm[..32])
        };

        Ok(CryptoSession {
            tx: XChaCha20Poly1305::new(Key::from_slice(tx_key)),
            rx: XChaCha20Poly1305::new(Key::from_slice(rx_key)),
        })
    }

    /// Build a forward-secret session from the pinned identity DH and a fresh
    /// per-connection DH. The ephemeral secret must be discarded after this
    /// returns. Including both public keys in HKDF binds the result to this
    /// handshake and keeps both peers' send/receive directions aligned.
    pub fn session_with_ephemeral(
        &self,
        remote_identity_b64: &str,
        local_ephemeral: &StaticSecret,
        remote_ephemeral_b64: &str,
    ) -> Result<CryptoSession, String> {
        let remote_identity = decode_public_key(remote_identity_b64)?;
        let remote_ephemeral = decode_public_key(remote_ephemeral_b64)?;
        let identity_shared = self.secret.diffie_hellman(&remote_identity);
        let ephemeral_shared = local_ephemeral.diffie_hellman(&remote_ephemeral);
        if identity_shared.as_bytes().iter().all(|b| *b == 0)
            || ephemeral_shared.as_bytes().iter().all(|b| *b == 0)
        {
            return Err("invalid X25519 public key".to_string());
        }

        let local_identity = self.public.as_bytes();
        let remote_identity = remote_identity.as_bytes();
        let local_ephemeral_public = PublicKey::from(local_ephemeral);
        let local_ephemeral = local_ephemeral_public.as_bytes();
        let remote_ephemeral = remote_ephemeral.as_bytes();
        let mut salt = Vec::with_capacity(128);
        if local_identity <= remote_identity {
            salt.extend_from_slice(local_identity);
            salt.extend_from_slice(remote_identity);
        } else {
            salt.extend_from_slice(remote_identity);
            salt.extend_from_slice(local_identity);
        }
        if local_ephemeral <= remote_ephemeral {
            salt.extend_from_slice(local_ephemeral);
            salt.extend_from_slice(remote_ephemeral);
        } else {
            salt.extend_from_slice(remote_ephemeral);
            salt.extend_from_slice(local_ephemeral);
        }

        let mut ikm = [0u8; 64];
        ikm[..32].copy_from_slice(identity_shared.as_bytes());
        ikm[32..].copy_from_slice(ephemeral_shared.as_bytes());
        let hk = Hkdf::<Sha256>::new(Some(&salt), &ikm);
        let mut okm = [0u8; 64];
        hk.expand(b"forge-net peer session v2", &mut okm)
            .map_err(|_| "key derivation failed".to_string())?;

        let local_is_low = local_identity <= remote_identity;
        let (tx_key, rx_key) = if local_is_low {
            (&okm[..32], &okm[32..])
        } else {
            (&okm[32..], &okm[..32])
        };
        Ok(CryptoSession {
            tx: XChaCha20Poly1305::new(Key::from_slice(tx_key)),
            rx: XChaCha20Poly1305::new(Key::from_slice(rx_key)),
        })
    }

    pub fn new_ephemeral() -> (StaticSecret, String) {
        let secret = StaticSecret::random_from_rng(OsRng);
        let public = PublicKey::from(&secret);
        (secret, STANDARD.encode(public.as_bytes()))
    }
}

impl ContactBook {
    pub fn load(dir: &Path) -> io::Result<Self> {
        fs::create_dir_all(dir)?;
        let path = dir.join(CONTACTS_FILE);
        let mut contacts = HashMap::new();
        if let Ok(bytes) = fs::read(&path) {
            let parsed: ContactFile = serde_json::from_slice(&bytes)
                .map_err(|e| io::Error::new(io::ErrorKind::InvalidData, e))?;
            for contact in parsed.contacts {
                contacts.insert(contact.handle.clone(), contact);
            }
        }
        Ok(Self { path, contacts, pending: HashMap::new() })
    }

    pub fn verify_or_trust(
        &mut self,
        handle: &str,
        guild: &str,
        public_key_b64: &str,
        addr: Option<String>,
        relay: Option<String>,
    ) -> Result<TrustDecision, String> {
        let public = decode_public_key(public_key_b64)?;
        let fp = fingerprint(public.as_bytes());
        let candidate = Contact {
            handle: handle.to_string(),
            guild: guild.to_string(),
            public_key_b64: public_key_b64.to_string(),
            fingerprint: fp.clone(),
            addr: addr.clone(),
            relay: relay.clone(),
        };

        if let Some(existing) = self.contacts.get(handle) {
            if existing.fingerprint != fp {
                self.pending.insert(handle.to_string(), candidate);
                return Ok(TrustDecision::ChangedKey {
                    expected: existing.fingerprint.clone(),
                    received: fp,
                });
            }
        }
        if self.contacts.contains_key(handle) {
            let existing = self.contacts.get_mut(handle).expect("contact checked above");
            if addr.is_some() {
                existing.addr = addr;
            }
            if relay.is_some() {
                existing.relay = relay;
            }
            self.save().map_err(|e| e.to_string())?;
            return Ok(TrustDecision::Trusted);
        }

        self.pending.insert(handle.to_string(), candidate);
        Ok(TrustDecision::PendingTrust { fingerprint: fp })
    }

    pub fn accept_pending_fingerprint(&mut self, expected_fingerprint: &str) -> Result<Contact, String> {
        let handle = self.pending.iter()
            .find(|(_, contact)| contact.fingerprint == expected_fingerprint)
            .map(|(handle, _)| handle.clone())
            .ok_or_else(|| "no pending peer has that fingerprint".to_string())?;
        let contact = self.pending.remove(&handle).expect("pending contact checked above");
        self.contacts.insert(handle, contact.clone());
        self.save().map_err(|e| e.to_string())?;
        Ok(contact)
    }

    pub fn add_invite(&mut self, invite: Invite) -> Result<(), String> {
        let public = decode_public_key(&invite.public_key_b64)?;
        if fingerprint(public.as_bytes()) != invite.fingerprint {
            return Err("invite fingerprint does not match its public key".to_string());
        }
        if let Some(existing) = self.contacts.get(&invite.handle) {
            if existing.fingerprint != invite.fingerprint {
                return Err(format!(
                    "invite key for '{}' differs from the trusted fingerprint; approve a key change explicitly",
                    invite.handle
                ));
            }
        }
        self.contacts.insert(
            invite.handle.clone(),
            Contact {
                handle: invite.handle,
                guild: invite.guild,
                public_key_b64: invite.public_key_b64,
                fingerprint: invite.fingerprint,
                addr: invite.addr,
                relay: invite.relay,
            },
        );
        self.save().map_err(|e| e.to_string())
    }

    pub fn get(&self, handle: &str) -> Option<Contact> {
        self.contacts.get(handle).cloned()
    }

    pub fn pending_fingerprint(&self, handle: &str) -> Option<&str> {
        self.pending.get(handle).map(|contact| contact.fingerprint.as_str())
    }

    pub fn save(&self) -> io::Result<()> {
        let mut contacts: Vec<_> = self.contacts.values().cloned().collect();
        contacts.sort_by(|a, b| a.handle.cmp(&b.handle));
        let saved = ContactFile { contacts };
        fs::write(
            &self.path,
            serde_json::to_vec_pretty(&saved)
                .map_err(|e| io::Error::new(io::ErrorKind::InvalidData, e))?,
        )?;
        restrict_file_permissions(&self.path);
        Ok(())
    }
}

impl Invite {
    pub fn encode(&self) -> Result<String, String> {
        serde_json::to_vec(self)
            .map(|bytes| URL_SAFE_NO_PAD.encode(bytes))
            .map_err(|e| e.to_string())
    }

    pub fn decode(code: &str) -> Result<Self, String> {
        let bytes = URL_SAFE_NO_PAD.decode(code).map_err(|e| e.to_string())?;
        serde_json::from_slice(&bytes).map_err(|e| e.to_string())
    }
}

impl CryptoSession {
    pub fn encrypt(&mut self, plaintext: &[u8]) -> Result<EncryptedFrame, String> {
        // Frames can share a session key, so every frame uses a fresh 192-bit
        // nonce. XChaCha20's large nonce makes random generation appropriate.
        let mut nonce = [0u8; 24];
        OsRng.fill_bytes(&mut nonce);
        let body = self
            .tx
            .encrypt(XNonce::from_slice(&nonce), plaintext)
            .map_err(|_| "encryption failed".to_string())?;
        Ok(EncryptedFrame {
            nonce_b64: STANDARD.encode(nonce),
            body_b64: STANDARD.encode(body),
        })
    }

    pub fn decrypt(&self, frame: &EncryptedFrame) -> Result<Vec<u8>, String> {
        let nonce = STANDARD
            .decode(&frame.nonce_b64)
            .map_err(|e| e.to_string())?;
        let nonce: [u8; 24] = nonce
            .try_into()
            .map_err(|_| "bad nonce length".to_string())?;
        let body = STANDARD
            .decode(&frame.body_b64)
            .map_err(|e| e.to_string())?;
        self.rx
            .decrypt(XNonce::from_slice(&nonce), body.as_ref())
            .map_err(|_| "decryption failed".to_string())
    }
}

pub fn fingerprint(public_key: &[u8]) -> String {
    let digest = Sha256::digest(public_key);
    let mut out = String::with_capacity(47);
    for (idx, byte) in digest[..16].iter().enumerate() {
        if idx > 0 {
            out.push(':');
        }
        out.push_str(&format!("{byte:02x}"));
    }
    out
}

pub fn decode_public_key(public_key_b64: &str) -> Result<PublicKey, String> {
    let bytes = STANDARD.decode(public_key_b64).map_err(|e| e.to_string())?;
    let bytes: [u8; 32] = bytes
        .try_into()
        .map_err(|_| "bad public key length".to_string())?;
    Ok(PublicKey::from(bytes))
}

fn restrict_file_permissions(path: &Path) {
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        let _ = fs::set_permissions(path, fs::Permissions::from_mode(0o600));
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn test_dir() -> PathBuf {
        let nonce = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH).unwrap()
            .as_nanos();
        std::env::temp_dir().join(format!("forge-crypto-{}-{nonce}", std::process::id()))
    }

    fn public_key(seed: u8) -> String {
        let secret = StaticSecret::from([seed; 32]);
        let public = PublicKey::from(&secret);
        STANDARD.encode(public.as_bytes())
    }

    #[test]
    fn discovered_peer_requires_exact_fingerprint_approval() {
        let dir = test_dir();
        let mut book = ContactBook::load(&dir).unwrap();
        let key = public_key(7);
        let public = decode_public_key(&key).unwrap();
        let fp = fingerprint(public.as_bytes());

        assert!(matches!(
            book.verify_or_trust("ada", "compiler", &key, Some("127.0.0.1:9".into()), None).unwrap(),
            TrustDecision::PendingTrust { .. }
        ));
        assert!(book.get("ada").is_none());
        assert!(book.accept_pending_fingerprint("00:00").is_err());
        let accepted = book.accept_pending_fingerprint(&fp).unwrap();
        assert_eq!(accepted.handle, "ada");
        assert!(matches!(
            book.verify_or_trust("ada", "compiler", &key, None, None).unwrap(),
            TrustDecision::Trusted
        ));

        drop(book);
        let _ = fs::remove_dir_all(dir);
    }

    #[test]
    fn invite_rejects_fingerprint_key_mismatch() {
        let dir = test_dir();
        let mut book = ContactBook::load(&dir).unwrap();
        let invite = Invite {
            handle: "ada".into(), guild: "compiler".into(),
            public_key_b64: public_key(3), fingerprint: "wrong".into(),
            addr: None, relay: None,
            signal_url: None, signal_room: None,
        };
        assert!(book.add_invite(invite).is_err());
        assert!(book.get("ada").is_none());
        drop(book);
        let _ = fs::remove_dir_all(dir);
    }

    #[test]
    fn invite_cannot_replace_an_already_trusted_identity_key() {
        let dir = test_dir();
        let mut book = ContactBook::load(&dir).unwrap();
        let trusted_key = public_key(9);
        let trusted_fp = fingerprint(decode_public_key(&trusted_key).unwrap().as_bytes());
        book.add_invite(Invite {
            handle: "ada".into(),
            guild: "compiler".into(),
            public_key_b64: trusted_key,
            fingerprint: trusted_fp.clone(),
            addr: None,
            relay: None,
            signal_url: None,
            signal_room: None,
        }).unwrap();

        let replacement_key = public_key(10);
        let replacement_fp = fingerprint(decode_public_key(&replacement_key).unwrap().as_bytes());
        let result = book.add_invite(Invite {
            handle: "ada".into(),
            guild: "compiler".into(),
            public_key_b64: replacement_key,
            fingerprint: replacement_fp,
            addr: None,
            relay: None,
            signal_url: None,
            signal_room: None,
        });
        assert!(result.is_err());
        assert_eq!(book.get("ada").unwrap().fingerprint, trusted_fp);
        drop(book);
        let _ = fs::remove_dir_all(dir);
    }

    #[test]
    fn ephemeral_dh_keys_produce_matching_directional_sessions() {
        let a_secret = StaticSecret::from([11; 32]);
        let a = Identity { public: PublicKey::from(&a_secret), secret: a_secret };
        let b_secret = StaticSecret::from([22; 32]);
        let b = Identity { public: PublicKey::from(&b_secret), secret: b_secret };
        let (a_eph, a_pub) = Identity::new_ephemeral();
        let (b_eph, b_pub) = Identity::new_ephemeral();
        let mut a_session = a.session_with_ephemeral(&b.public_key_b64(), &a_eph, &b_pub).unwrap();
        let b_session = b.session_with_ephemeral(&a.public_key_b64(), &b_eph, &a_pub).unwrap();

        let frame = a_session.encrypt(b"forward secret").unwrap();
        assert_eq!(b_session.decrypt(&frame).unwrap(), b"forward secret");
    }
}
