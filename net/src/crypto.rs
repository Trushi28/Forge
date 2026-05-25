//! Persistent identity, TOFU contact pinning, and peer-frame encryption.

use base64::Engine;
use base64::engine::general_purpose::{STANDARD, URL_SAFE_NO_PAD};
use chacha20poly1305::aead::{Aead, KeyInit};
use chacha20poly1305::{Key, XChaCha20Poly1305, XNonce};
use hkdf::Hkdf;
use rand_core::OsRng;
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
}

#[derive(Default, Serialize, Deserialize)]
struct ContactFile {
    contacts: Vec<Contact>,
}

pub struct ContactBook {
    path: PathBuf,
    contacts: HashMap<String, Contact>,
}

#[derive(Debug)]
pub enum TrustDecision {
    Trusted,
    NewTrusted,
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
}

pub struct CryptoSession {
    tx: XChaCha20Poly1305,
    rx: XChaCha20Poly1305,
    send_counter: u64,
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
            send_counter: 0,
        })
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
        Ok(Self { path, contacts })
    }

    pub fn verify_or_trust(
        &mut self,
        handle: &str,
        guild: &str,
        public_key_b64: &str,
        addr: Option<String>,
    ) -> Result<TrustDecision, String> {
        let public = decode_public_key(public_key_b64)?;
        let fp = fingerprint(public.as_bytes());

        if let Some(existing) = self.contacts.get(handle) {
            if existing.fingerprint != fp {
                return Ok(TrustDecision::ChangedKey {
                    expected: existing.fingerprint.clone(),
                    received: fp,
                });
            }
            return Ok(TrustDecision::Trusted);
        }

        self.contacts.insert(
            handle.to_string(),
            Contact {
                handle: handle.to_string(),
                guild: guild.to_string(),
                public_key_b64: public_key_b64.to_string(),
                fingerprint: fp,
                addr,
            },
        );
        self.save().map_err(|e| e.to_string())?;
        Ok(TrustDecision::NewTrusted)
    }

    pub fn add_invite(&mut self, invite: Invite) -> Result<(), String> {
        decode_public_key(&invite.public_key_b64)?;
        self.contacts.insert(
            invite.handle.clone(),
            Contact {
                handle: invite.handle,
                guild: invite.guild,
                public_key_b64: invite.public_key_b64,
                fingerprint: invite.fingerprint,
                addr: invite.addr,
            },
        );
        self.save().map_err(|e| e.to_string())
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
        let mut nonce = [0u8; 24];
        nonce[..8].copy_from_slice(&self.send_counter.to_be_bytes());
        self.send_counter = self.send_counter.saturating_add(1);
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
