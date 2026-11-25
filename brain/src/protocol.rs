use bytes::{BufMut, BytesMut};
use ring::hmac;
use thiserror::Error;
use tokio::io::{AsyncRead, AsyncReadExt, AsyncWrite, AsyncWriteExt};

pub const FRAME_MAGIC: [u8; 2] = [0x47, 0x42]; // "GB"
pub const HEADER_LEN: usize = 6;
pub const MAX_PAYLOAD_LEN: usize = 4096;
pub const PROTO_VERSION: u8 = 1;
pub const AUTH_NONCE_LEN: usize = 16;
pub const AUTH_HMAC_LEN: usize = 32;

// Body -> Brain message types
pub const MSG_HELLO: u8 = 0x01;
pub const MSG_AUDIO_UP: u8 = 0x02;
pub const MSG_WAKE: u8 = 0x03;
pub const MSG_BARGE_IN: u8 = 0x04;
pub const MSG_SLEEP: u8 = 0x05;
pub const MSG_PING: u8 = 0x06;
pub const MSG_AUTH_RESPONSE: u8 = 0x07;

// Brain -> Body message types
pub const MSG_WELCOME: u8 = 0x81;
pub const MSG_AUDIO_DOWN: u8 = 0x82;
pub const MSG_FACE_STATE: u8 = 0x83;
pub const MSG_STOP_AUDIO: u8 = 0x84;
pub const MSG_CONFIG: u8 = 0x85;
pub const MSG_PONG: u8 = 0x86;
pub const MSG_AUTH_CHALLENGE: u8 = 0x87;

/// Computes HMAC-SHA256(token, domain || nonce_brain || nonce_body).
pub fn compute_auth_mac(
    token: &str,
    domain: &[u8],
    nonce_brain: &[u8; AUTH_NONCE_LEN],
    nonce_body: &[u8; AUTH_NONCE_LEN],
) -> [u8; AUTH_HMAC_LEN] {
    let key = hmac::Key::new(hmac::HMAC_SHA256, token.as_bytes());
    let mut ctx = hmac::Context::with_key(&key);
    ctx.update(domain);
    ctx.update(nonce_brain);
    ctx.update(nonce_body);
    let tag = ctx.sign();
    let mut out = [0u8; AUTH_HMAC_LEN];
    out.copy_from_slice(tag.as_ref());
    out
}

/// Verifies HMAC-SHA256(token, domain || nonce_brain || nonce_body) in constant time.
pub fn verify_auth_mac(
    token: &str,
    domain: &[u8],
    nonce_brain: &[u8; AUTH_NONCE_LEN],
    nonce_body: &[u8; AUTH_NONCE_LEN],
    received: &[u8; AUTH_HMAC_LEN],
) -> bool {
    let key = hmac::Key::new(hmac::HMAC_SHA256, token.as_bytes());
    let mut msg = Vec::with_capacity(domain.len() + AUTH_NONCE_LEN * 2);
    msg.extend_from_slice(domain);
    msg.extend_from_slice(nonce_brain);
    msg.extend_from_slice(nonce_body);
    hmac::verify(&key, &msg, received).is_ok()
}

/// Mirrors `face_emotion_t` in firmware/main/ui/face/face.h
#[repr(u8)]
#[allow(dead_code)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Emotion {
    Idle = 0,
    Listening = 1,
    Focusing = 2,
    Thinking = 3,
    Speaking = 4,
    Happy = 5,
    Curious = 6,
    Concerned = 7,
    Sleepy = 8,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum WakeSource {
    WakeWord,
    TouchOrButton,
    Unknown(u8),
}

impl From<u8> for WakeSource {
    fn from(value: u8) -> Self {
        match value {
            1 => Self::WakeWord,
            2 => Self::TouchOrButton,
            other => Self::Unknown(other),
        }
    }
}

impl From<WakeSource> for u8 {
    fn from(value: WakeSource) -> Self {
        match value {
            WakeSource::WakeWord => 1,
            WakeSource::TouchOrButton => 2,
            WakeSource::Unknown(v) => v,
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum BodyMessage {
    Hello {
        proto_version: u8,
        sample_rate_khz: u8,
        channels: u8,
        caps_flags: u8,
        device_id: String,
    },
    AuthResponse {
        nonce_body: [u8; AUTH_NONCE_LEN],
        hmac_body: [u8; AUTH_HMAC_LEN],
    },
    AudioUp(Vec<u8>),
    Wake {
        source: WakeSource,
    },
    BargeIn {
        source: WakeSource,
    },
    Sleep,
    Ping {
        seq: u32,
    },
}

#[allow(dead_code)]
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum BrainMessage {
    AuthChallenge {
        nonce_brain: [u8; AUTH_NONCE_LEN],
    },
    Welcome {
        proto_version: u8,
        sample_rate_khz: u8,
        hmac_brain: Option<[u8; AUTH_HMAC_LEN]>,
    },
    AudioDown(Vec<u8>),
    FaceState {
        emotion: Emotion,
        energy: u8,
        yaw: i8,
        pitch: i8,
    },
    StopAudio,
    Config {
        volume: Option<u8>,
        brightness: Option<u8>,
    },
    Pong {
        seq: u32,
    },
}

#[derive(Debug, Error)]
pub enum ProtocolError {
    #[error("I/O error: {0}")]
    Io(#[from] std::io::Error),
    #[error("invalid frame magic: {0:02x?}")]
    InvalidMagic([u8; 2]),
    #[error("payload length {0} exceeds max {MAX_PAYLOAD_LEN}")]
    PayloadTooLarge(usize),
    #[error("unknown message type: 0x{0:02x}")]
    UnknownMessageType(u8),
    #[error("malformed payload for message 0x{0:02x}: {1}")]
    MalformedPayload(u8, &'static str),
}

pub fn encode_raw(msg_type: u8, flags: u8, payload: &[u8]) -> Result<Vec<u8>, ProtocolError> {
    if payload.len() > MAX_PAYLOAD_LEN {
        return Err(ProtocolError::PayloadTooLarge(payload.len()));
    }
    let mut buf = BytesMut::with_capacity(HEADER_LEN + payload.len());
    buf.put_slice(&FRAME_MAGIC);
    buf.put_u8(msg_type);
    buf.put_u8(flags);
    buf.put_u16_le(payload.len() as u16);
    buf.put_slice(payload);
    Ok(buf.to_vec())
}

pub fn encode_brain_message(msg: &BrainMessage) -> Result<Vec<u8>, ProtocolError> {
    match msg {
        BrainMessage::AuthChallenge { nonce_brain } => encode_raw(MSG_AUTH_CHALLENGE, 1, nonce_brain),
        BrainMessage::Welcome {
            proto_version,
            sample_rate_khz,
            hmac_brain,
        } => {
            if let Some(mac) = hmac_brain {
                let mut payload = Vec::with_capacity(2 + AUTH_HMAC_LEN);
                payload.push(*proto_version);
                payload.push(*sample_rate_khz);
                payload.extend_from_slice(mac);
                encode_raw(MSG_WELCOME, 0, &payload)
            } else {
                encode_raw(MSG_WELCOME, 0, &[*proto_version, *sample_rate_khz])
            }
        }
        BrainMessage::AudioDown(pcm) => encode_raw(MSG_AUDIO_DOWN, 0, pcm),
        BrainMessage::FaceState {
            emotion,
            energy,
            yaw,
            pitch,
        } => encode_raw(
            MSG_FACE_STATE,
            0,
            &[*emotion as u8, *energy, *yaw as u8, *pitch as u8],
        ),
        BrainMessage::StopAudio => encode_raw(MSG_STOP_AUDIO, 1, &[]),
        BrainMessage::Config { volume, brightness } => {
            let v = volume.unwrap_or(0xFF);
            let b = brightness.unwrap_or(0xFF);
            encode_raw(MSG_CONFIG, 0, &[v, b])
        }
        BrainMessage::Pong { seq } => encode_raw(MSG_PONG, 0, &seq.to_le_bytes()),
    }
}

pub fn decode_body_message(msg_type: u8, payload: Vec<u8>) -> Result<BodyMessage, ProtocolError> {
    match msg_type {
        MSG_HELLO => {
            if payload.len() < 4 {
                return Err(ProtocolError::MalformedPayload(
                    MSG_HELLO,
                    "expected at least 4 bytes",
                ));
            }
            let device_id = String::from_utf8_lossy(&payload[4..]).into_owned();
            Ok(BodyMessage::Hello {
                proto_version: payload[0],
                sample_rate_khz: payload[1],
                channels: payload[2],
                caps_flags: payload[3],
                device_id,
            })
        }
        MSG_AUTH_RESPONSE => {
            if payload.len() != AUTH_NONCE_LEN + AUTH_HMAC_LEN {
                return Err(ProtocolError::MalformedPayload(
                    MSG_AUTH_RESPONSE,
                    "expected 16-byte nonce + 32-byte HMAC",
                ));
            }
            let mut nonce_body = [0u8; AUTH_NONCE_LEN];
            let mut hmac_body = [0u8; AUTH_HMAC_LEN];
            nonce_body.copy_from_slice(&payload[..AUTH_NONCE_LEN]);
            hmac_body.copy_from_slice(&payload[AUTH_NONCE_LEN..]);
            Ok(BodyMessage::AuthResponse {
                nonce_body,
                hmac_body,
            })
        }
        MSG_AUDIO_UP => Ok(BodyMessage::AudioUp(payload)),
        MSG_WAKE => {
            let src = payload.first().copied().unwrap_or(0);
            Ok(BodyMessage::Wake {
                source: WakeSource::from(src),
            })
        }
        MSG_BARGE_IN => {
            let src = payload.first().copied().unwrap_or(0);
            Ok(BodyMessage::BargeIn {
                source: WakeSource::from(src),
            })
        }
        MSG_SLEEP => Ok(BodyMessage::Sleep),
        MSG_PING => {
            if payload.len() < 4 {
                return Err(ProtocolError::MalformedPayload(
                    MSG_PING,
                    "expected 4-byte sequence number",
                ));
            }
            let seq = u32::from_le_bytes([payload[0], payload[1], payload[2], payload[3]]);
            Ok(BodyMessage::Ping { seq })
        }
        other => Err(ProtocolError::UnknownMessageType(other)),
    }
}

pub async fn read_body_message<R: AsyncRead + Unpin>(
    reader: &mut R,
) -> Result<BodyMessage, ProtocolError> {
    let mut header = [0u8; HEADER_LEN];
    reader.read_exact(&mut header).await?;

    let magic = [header[0], header[1]];
    if magic != FRAME_MAGIC {
        return Err(ProtocolError::InvalidMagic(magic));
    }

    let msg_type = header[2];
    let len = u16::from_le_bytes([header[4], header[5]]) as usize;
    if len > MAX_PAYLOAD_LEN {
        return Err(ProtocolError::PayloadTooLarge(len));
    }

    let mut payload = vec![0u8; len];
    if len > 0 {
        reader.read_exact(&mut payload).await?;
    }

    decode_body_message(msg_type, payload)
}

pub async fn write_brain_message<W: AsyncWrite + Unpin>(
    writer: &mut W,
    msg: &BrainMessage,
) -> Result<(), ProtocolError> {
    let frame = encode_brain_message(msg)?;
    writer.write_all(&frame).await?;
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::Cursor;

    #[tokio::test]
    async fn roundtrip_hello_and_ping() {
        let mut payload = vec![PROTO_VERSION, 16, 1, 0b11];
        payload.extend_from_slice(b"vocat-desk");
        let raw_hello = encode_raw(MSG_HELLO, 0, &payload).unwrap();
        let raw_ping = encode_raw(MSG_PING, 0, &42u32.to_le_bytes()).unwrap();

        let mut stream = Vec::new();
        stream.extend_from_slice(&raw_hello);
        stream.extend_from_slice(&raw_ping);

        let mut cursor = Cursor::new(stream);
        let msg1 = read_body_message(&mut cursor).await.unwrap();
        assert_eq!(
            msg1,
            BodyMessage::Hello {
                proto_version: PROTO_VERSION,
                sample_rate_khz: 16,
                channels: 1,
                caps_flags: 0b11,
                device_id: "vocat-desk".to_string(),
            }
        );

        let msg2 = read_body_message(&mut cursor).await.unwrap();
        assert_eq!(msg2, BodyMessage::Ping { seq: 42 });
    }

    #[test]
    fn encodes_face_state_and_stop_audio() {
        let frame = encode_brain_message(&BrainMessage::FaceState {
            emotion: Emotion::Speaking,
            energy: 180,
            yaw: -10,
            pitch: 5,
        })
        .unwrap();

        assert_eq!(&frame[0..2], &FRAME_MAGIC);
        assert_eq!(frame[2], MSG_FACE_STATE);
        assert_eq!(u16::from_le_bytes([frame[4], frame[5]]), 4);
        assert_eq!(frame[6], Emotion::Speaking as u8);
        assert_eq!(frame[7], 180);
        assert_eq!(frame[8] as i8, -10);
        assert_eq!(frame[9] as i8, 5);

        let stop = encode_brain_message(&BrainMessage::StopAudio).unwrap();
        assert_eq!(stop, vec![0x47, 0x42, MSG_STOP_AUDIO, 1, 0, 0]);
    }

    #[tokio::test]
    async fn rejects_bad_magic_and_oversize() {
        let mut bad = Cursor::new(vec![0x00, 0x00, MSG_SLEEP, 0, 0, 0]);
        assert!(matches!(
            read_body_message(&mut bad).await,
            Err(ProtocolError::InvalidMagic([0, 0]))
        ));

        let huge_len = (MAX_PAYLOAD_LEN as u16 + 1).to_le_bytes();
        let mut oversize = Cursor::new(vec![0x47, 0x42, MSG_AUDIO_UP, 0, huge_len[0], huge_len[1]]);
        assert!(matches!(
            read_body_message(&mut oversize).await,
            Err(ProtocolError::PayloadTooLarge(_))
        ));
    }

    #[test]
    fn hmac_auth_verify_roundtrip() {
        let nb = [1u8; AUTH_NONCE_LEN];
        let nd = [2u8; AUTH_NONCE_LEN];
        let mac = compute_auth_mac("super-secret-token", b"GB1-BODY", &nb, &nd);
        assert!(verify_auth_mac("super-secret-token", b"GB1-BODY", &nb, &nd, &mac));
        assert!(!verify_auth_mac("wrong-token", b"GB1-BODY", &nb, &nd, &mac));
        assert!(!verify_auth_mac("super-secret-token", b"GB1-BRAIN", &nb, &nd, &mac));
    }
}
