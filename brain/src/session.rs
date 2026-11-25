use crate::audio::{pcm16_energy, PcmChunker};
use crate::config::Config;
use crate::protocol::{
    compute_auth_mac, read_body_message, verify_auth_mac, write_brain_message, BodyMessage,
    BrainMessage, Emotion, AUTH_NONCE_LEN, PROTO_VERSION,
};
use crate::xai::{XaiCommand, XaiEvent, XaiSession};
use anyhow::{anyhow, Result};
use ring::rand::{SecureRandom, SystemRandom};
use std::collections::VecDeque;
use std::sync::Arc;
use std::time::{Duration, Instant};
use tokio::io::{AsyncRead, AsyncWrite};
use tokio::sync::{mpsc, watch, OwnedSemaphorePermit};
use tokio::time::{timeout, MissedTickBehavior};
use tracing::{debug, error, info, warn};

/// Number of 20 ms frames sent immediately at utterance start to fill the Body jitter buffer.
const INITIAL_PREFILL_FRAMES: usize = 6;
/// Pacing interval for 20 ms PCM frames (slightly faster than 20.0 ms to prevent I2S underrun).
const FRAME_PACE_US: u64 = 19_800;
/// Maximum time allowed for HELLO + mutual HMAC-SHA256 handshake.
const HANDSHAKE_TIMEOUT: Duration = Duration::from_secs(5);

const AUTH_DOMAIN_BODY: &[u8] = b"GB1-BODY";
const AUTH_DOMAIN_BRAIN: &[u8] = b"GB1-BRAIN";

/// Performs strict HELLO + optional mutual HMAC-SHA256 challenge-response before any
/// audio or xAI session is permitted.
async fn perform_handshake<R, W>(
    reader: &mut R,
    writer: &mut W,
    peer_label: &str,
    cfg: &Config,
) -> Result<()>
where
    R: AsyncRead + Unpin,
    W: AsyncWrite + Unpin,
{
    let first = timeout(HANDSHAKE_TIMEOUT, read_body_message(reader))
        .await
        .map_err(|_| anyhow!("handshake timed out waiting for HELLO"))??;

    let (proto_version, sample_rate_khz, channels, caps_flags, device_id) = match first {
        BodyMessage::Hello {
            proto_version,
            sample_rate_khz,
            channels,
            caps_flags,
            device_id,
        } => (
            proto_version,
            sample_rate_khz,
            channels,
            caps_flags,
            device_id,
        ),
        other => {
            return Err(anyhow!(
                "rejected connection: expected HELLO before {other:?}"
            ));
        }
    };

    let token = cfg.auth_token.trim();
    let hmac_brain = if !token.is_empty() {
        let rng = SystemRandom::new();
        let mut nonce_brain = [0u8; AUTH_NONCE_LEN];
        rng.fill(&mut nonce_brain)
            .map_err(|_| anyhow!("CSPRNG failure generating auth challenge"))?;

        timeout(
            HANDSHAKE_TIMEOUT,
            write_brain_message(writer, &BrainMessage::AuthChallenge { nonce_brain }),
        )
        .await
        .map_err(|_| anyhow!("handshake timed out sending AUTH_CHALLENGE"))??;

        let resp = timeout(HANDSHAKE_TIMEOUT, read_body_message(reader))
            .await
            .map_err(|_| anyhow!("handshake timed out waiting for AUTH_RESPONSE"))??;

        let (nonce_body, hmac_body) = match resp {
            BodyMessage::AuthResponse {
                nonce_body,
                hmac_body,
            } => (nonce_body, hmac_body),
            other => {
                return Err(anyhow!(
                    "rejected unauthenticated peer: expected AUTH_RESPONSE, got {other:?}"
                ));
            }
        };

        if !verify_auth_mac(token, AUTH_DOMAIN_BODY, &nonce_brain, &nonce_body, &hmac_body) {
            return Err(anyhow!("Body HMAC-SHA256 authentication failed"));
        }

        Some(compute_auth_mac(
            token,
            AUTH_DOMAIN_BRAIN,
            &nonce_brain,
            &nonce_body,
        ))
    } else if cfg.allow_unauthenticated {
        None
    } else {
        return Err(anyhow!(
            "authentication required but GROKBOT_AUTH_TOKEN is empty"
        ));
    };

    timeout(
        HANDSHAKE_TIMEOUT,
        write_brain_message(
            writer,
            &BrainMessage::Welcome {
                proto_version: PROTO_VERSION,
                sample_rate_khz: 16,
                hmac_brain,
            },
        ),
    )
    .await
    .map_err(|_| anyhow!("handshake timed out sending WELCOME"))??;

    timeout(
        HANDSHAKE_TIMEOUT,
        write_brain_message(
            writer,
            &BrainMessage::FaceState {
                emotion: Emotion::Idle,
                energy: 0,
                yaw: 0,
                pitch: 0,
            },
        ),
    )
    .await
    .map_err(|_| anyhow!("handshake timed out sending initial FACE_STATE"))??;

    info!(
        peer = %peer_label,
        proto_version,
        sample_rate_khz,
        channels,
        caps_flags,
        %device_id,
        authenticated = hmac_brain.is_some(),
        "ESP-VoCat Body connected"
    );

    Ok(())
}

/// Bridges one connected ESP-VoCat Body stream (TCP today, USB serial tomorrow)
/// with an xAI Realtime Voice session.
pub async fn handle_body_stream<S>(
    stream: S,
    peer_label: String,
    cfg: Config,
    active_gen_tx: Arc<watch::Sender<u64>>,
    handshake_permit: OwnedSemaphorePermit,
) -> Result<()>
where
    S: AsyncRead + AsyncWrite + Unpin + Send + 'static,
{
    let (mut reader, mut writer) = tokio::io::split(stream);

    // Complete HELLO + mutual auth before claiming the active session slot.
    perform_handshake(&mut reader, &mut writer, &peer_label, &cfg).await?;
    // Handshake complete; release handshake concurrency permit.
    drop(handshake_permit);

    // Supersede any stale previous Body connection (for example, after an ESP32 reboot).
    let my_gen = {
        let next = *active_gen_tx.borrow() + 1;
        let _ = active_gen_tx.send(next);
        next
    };
    let mut active_gen_rx = active_gen_tx.subscribe();

    let (tx_body, mut rx_body) = mpsc::channel::<BrainMessage>(256);

    // Dedicated writer task so outgoing audio and face state never stall reads.
    let writer_task = tokio::spawn(async move {
        while let Some(msg) = rx_body.recv().await {
            if let Err(e) = write_brain_message(&mut writer, &msg).await {
                warn!("body write failed: {e}");
                break;
            }
        }
    });

    // Dedicated reader task turning framed bytes into BodyMessage events.
    let (tx_in, mut rx_in) = mpsc::channel::<BodyMessage>(256);
    let reader_task = tokio::spawn(async move {
        loop {
            match read_body_message(&mut reader).await {
                Ok(msg) => {
                    if tx_in.send(msg).await.is_err() {
                        break;
                    }
                }
                Err(e) => {
                    info!("body stream ended: {e}");
                    break;
                }
            }
        }
    });

    let mut awake = false;
    let mut speaking = false;
    let mut response_done_pending = false;
    let mut prefill_remaining: usize = 0;
    let mut xai: Option<XaiSession> = None;
    let mut conversation_id: Option<String> = None;
    let mut chunker = PcmChunker::new();
    let mut play_queue: VecDeque<Vec<u8>> = VecDeque::with_capacity(512);
    let mut last_face_energy_at = Instant::now() - Duration::from_secs(1);
    let mut mic_frames: u64 = 0;
    let mut mic_max_energy: u8 = 0;
    let mut last_mic_log_at = Instant::now();
    let mut audio_down_bytes: usize = 0;

    let mut pace_tick = tokio::time::interval(Duration::from_micros(FRAME_PACE_US));
    pace_tick.set_missed_tick_behavior(MissedTickBehavior::Delay);

    loop {
        tokio::select! {
            changed = active_gen_rx.changed() => {
                if changed.is_ok() && *active_gen_rx.borrow() != my_gen {
                    info!(peer = %peer_label, "superseded by newer authenticated Body connection");
                    break;
                }
            }

            maybe_body = rx_in.recv() => {
                let Some(msg) = maybe_body else {
                    break;
                };

                match msg {
                    BodyMessage::Hello { .. } | BodyMessage::AuthResponse { .. } => {
                        debug!(peer = %peer_label, "ignoring post-handshake control frame");
                    }

                    BodyMessage::Ping { seq } => {
                        let _ = tx_body.send(BrainMessage::Pong { seq }).await;
                    }

                    BodyMessage::Wake { source } => {
                        info!(peer = %peer_label, ?source, "Body woke up");
                        awake = true;
                        speaking = false;
                        response_done_pending = false;
                        prefill_remaining = 0;
                        chunker.clear();
                        play_queue.clear();

                        if xai.is_none() {
                            let _ = send_face(&tx_body, Emotion::Thinking, 0).await;
                            match XaiSession::connect(cfg.clone(), conversation_id.clone()).await {
                                Ok(session) => {
                                    xai = Some(session);
                                }
                                Err(e) => {
                                    error!("could not connect to xAI: {e}");
                                    let _ = send_face(&tx_body, Emotion::Concerned, 0).await;
                                    awake = false;
                                    continue;
                                }
                            }
                        }

                        let _ = send_face(&tx_body, Emotion::Listening, 0).await;
                    }

                    BodyMessage::BargeIn { source } => {
                        info!(peer = %peer_label, ?source, "Barge-in triggered");
                        awake = true;
                        speaking = false;
                        response_done_pending = false;
                        prefill_remaining = 0;
                        chunker.clear();
                        play_queue.clear();
                        let _ = tx_body.send(BrainMessage::StopAudio).await;
                        if let Some(ref session) = xai {
                            let _ = session.cmd_tx.send(XaiCommand::CancelResponse).await;
                        }
                        let _ = send_face(&tx_body, Emotion::Listening, 0).await;
                    }

                    BodyMessage::Sleep => {
                        info!(peer = %peer_label, "Body entered sleep");
                        awake = false;
                        speaking = false;
                        response_done_pending = false;
                        prefill_remaining = 0;
                        chunker.clear();
                        play_queue.clear();
                        let _ = tx_body.send(BrainMessage::StopAudio).await;
                        let _ = send_face(&tx_body, Emotion::Idle, 0).await;
                    }

                    BodyMessage::AudioUp(pcm) => {
                        if !awake {
                            continue;
                        }
                        mic_frames += 1;
                        let e = pcm16_energy(&pcm);
                        if e > mic_max_energy {
                            mic_max_energy = e;
                        }
                        if last_mic_log_at.elapsed() >= Duration::from_secs(2) {
                            debug!(
                                peer = %peer_label,
                                frames = mic_frames,
                                peak_energy = mic_max_energy,
                                "mic uplink active"
                            );
                            mic_frames = 0;
                            mic_max_energy = 0;
                            last_mic_log_at = Instant::now();
                        }
                        if let Some(ref session) = xai {
                            if session.cmd_tx.try_send(XaiCommand::SendPcm(pcm)).is_err() {
                                debug!("dropping mic frame due to xAI backpressure");
                            }
                        }
                    }
                }
            }

            maybe_xai = async {
                match xai.as_mut() {
                    Some(session) => session.evt_rx.recv().await,
                    None => std::future::pending().await,
                }
            } => {
                match maybe_xai {
                    Some(XaiEvent::SessionReady { conversation_id: cid }) => {
                        if cid.is_some() {
                            conversation_id = cid;
                        }
                        if awake {
                            let _ = send_face(&tx_body, Emotion::Listening, 0).await;
                        }
                    }
                    Some(XaiEvent::SpeechStarted) => {
                        info!(peer = %peer_label, "xAI VAD: speech started");
                        if speaking {
                            speaking = false;
                            response_done_pending = false;
                            prefill_remaining = 0;
                            chunker.clear();
                            play_queue.clear();
                            let _ = tx_body.send(BrainMessage::StopAudio).await;
                        }
                        if awake {
                            let _ = send_face(&tx_body, Emotion::Focusing, 0).await;
                        }
                    }
                    Some(XaiEvent::SpeechStopped) => {
                        info!(peer = %peer_label, "xAI VAD: speech stopped");
                        if awake {
                            let _ = send_face(&tx_body, Emotion::Thinking, 0).await;
                        }
                    }
                    Some(XaiEvent::ResponseStarted) => {
                        info!(peer = %peer_label, "xAI response started");
                        audio_down_bytes = 0;
                        response_done_pending = false;
                        if awake && !speaking {
                            let _ = send_face(&tx_body, Emotion::Thinking, 0).await;
                        }
                    }
                    Some(XaiEvent::AudioDelta(pcm)) => {
                        if !awake {
                            continue;
                        }
                        if !speaking {
                            speaking = true;
                            prefill_remaining = INITIAL_PREFILL_FRAMES;
                            info!(peer = %peer_label, bytes = pcm.len(), "xAI audio playback started");
                            let _ = send_face(&tx_body, Emotion::Speaking, 96).await;
                        }
                        audio_down_bytes += pcm.len();
                        for frame in chunker.push(&pcm) {
                            play_queue.push_back(frame);
                        }
                        while prefill_remaining > 0 {
                            let Some(frame) = play_queue.pop_front() else {
                                break;
                            };
                            prefill_remaining -= 1;
                            if tx_body.send(BrainMessage::AudioDown(frame)).await.is_err() {
                                break;
                            }
                        }
                    }
                    Some(XaiEvent::ResponseDone) => {
                        if let Some(tail) = chunker.flush() {
                            play_queue.push_back(tail);
                        }
                        info!(
                            peer = %peer_label,
                            total_bytes = audio_down_bytes,
                            queued_frames = play_queue.len(),
                            "xAI response done"
                        );
                        if play_queue.is_empty() {
                            speaking = false;
                            response_done_pending = false;
                            if awake {
                                let _ = send_face(&tx_body, Emotion::Listening, 0).await;
                            }
                        } else {
                            response_done_pending = true;
                        }
                    }
                    Some(XaiEvent::UserTranscript(t)) => {
                        if cfg.log_transcripts {
                            info!(peer = %peer_label, user = %t, "transcript");
                        } else {
                            debug!(peer = %peer_label, chars = t.len(), "user transcript received");
                        }
                    }
                    Some(XaiEvent::AssistantTranscript(t)) => {
                        if cfg.log_transcripts {
                            info!(peer = %peer_label, grok = %t, "transcript");
                        } else {
                            debug!(peer = %peer_label, chars = t.len(), "assistant transcript received");
                        }
                    }
                    Some(XaiEvent::Error(err)) => {
                        error!(peer = %peer_label, %err, "xAI server error");
                        let _ = send_face(&tx_body, Emotion::Concerned, 0).await;
                    }
                    Some(XaiEvent::Disconnected) | None => {
                        warn!(peer = %peer_label, "xAI session closed; will auto-resume on next wake");
                        xai = None;
                        speaking = false;
                        response_done_pending = false;
                        prefill_remaining = 0;
                        chunker.clear();
                        play_queue.clear();
                        if awake {
                            let _ = send_face(&tx_body, Emotion::Concerned, 0).await;
                            awake = false;
                        }
                    }
                }
            }

            _ = pace_tick.tick(), if !play_queue.is_empty() => {
                if let Some(frame) = play_queue.pop_front() {
                    if last_face_energy_at.elapsed() >= Duration::from_millis(50) {
                        let energy = pcm16_energy(&frame);
                        let _ = send_face(&tx_body, Emotion::Speaking, energy).await;
                        last_face_energy_at = Instant::now();
                    }
                    let _ = tx_body.send(BrainMessage::AudioDown(frame)).await;
                }
                if play_queue.is_empty() && response_done_pending {
                    speaking = false;
                    response_done_pending = false;
                    if awake {
                        let _ = send_face(&tx_body, Emotion::Listening, 0).await;
                    }
                }
            }
        }
    }

    if let Some(session) = xai {
        let _ = session.cmd_tx.send(XaiCommand::Shutdown).await;
    }
    reader_task.abort();
    drop(tx_body);
    let _ = writer_task.await;
    info!(peer = %peer_label, "ESP-VoCat Body disconnected");
    Ok(())
}

async fn send_face(tx: &mpsc::Sender<BrainMessage>, emotion: Emotion, energy: u8) -> Result<()> {
    tx.send(BrainMessage::FaceState {
        emotion,
        energy,
        yaw: 0,
        pitch: 0,
    })
    .await?;
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::protocol::{
        encode_raw, AUTH_HMAC_LEN, FRAME_MAGIC, HEADER_LEN, MSG_AUTH_CHALLENGE, MSG_AUTH_RESPONSE,
        MSG_FACE_STATE, MSG_HELLO, MSG_PING, MSG_PONG, MSG_WELCOME,
    };
    use tokio::io::{AsyncReadExt, AsyncWriteExt};
    use tokio::sync::Semaphore;

    async fn read_raw_frame<R: AsyncRead + Unpin>(r: &mut R) -> (u8, Vec<u8>) {
        let mut hdr = [0u8; HEADER_LEN];
        r.read_exact(&mut hdr).await.unwrap();
        assert_eq!(&hdr[0..2], &FRAME_MAGIC);
        let msg_type = hdr[2];
        let len = u16::from_le_bytes([hdr[4], hdr[5]]) as usize;
        let mut payload = vec![0u8; len];
        if len > 0 {
            r.read_exact(&mut payload).await.unwrap();
        }
        (msg_type, payload)
    }

    fn test_cfg(auth_token: &str, allow_unauthenticated: bool) -> Config {
        Config {
            xai_api_key: "xai-test".into(),
            listen: "127.0.0.1:0".parse().unwrap(),
            auth_token: auth_token.into(),
            allow_unauthenticated,
            log_transcripts: false,
            model: "grok-voice-latest".into(),
            voice: "eve".into(),
            instructions: "test".into(),
            reasoning_effort: "none".into(),
            web_search: false,
            x_search: false,
            mdns: false,
            mdns_name: "test".into(),
        }
    }

    #[tokio::test]
    async fn mutual_hmac_handshake_and_ping_over_duplex_stream() {
        let (mut client, server) = tokio::io::duplex(4096);
        let token = "test-pairing-secret-0123456789abcdef";
        let cfg = test_cfg(token, false);
        let (gen_tx, _gen_rx) = watch::channel(0u64);
        let sem = Arc::new(Semaphore::new(2));
        let permit = sem.try_acquire_owned().unwrap();

        let server_task = tokio::spawn(handle_body_stream(
            server,
            "test-body".into(),
            cfg,
            Arc::new(gen_tx),
            permit,
        ));

        let mut hello_payload = vec![PROTO_VERSION, 16, 1, 0b11];
        hello_payload.extend_from_slice(b"vocat-test");
        client
            .write_all(&encode_raw(MSG_HELLO, 0, &hello_payload).unwrap())
            .await
            .unwrap();

        let (t1, p1) = read_raw_frame(&mut client).await;
        assert_eq!(t1, MSG_AUTH_CHALLENGE);
        assert_eq!(p1.len(), AUTH_NONCE_LEN);
        let nonce_brain: [u8; AUTH_NONCE_LEN] = p1.try_into().unwrap();
        let nonce_body = [0x5Au8; AUTH_NONCE_LEN];
        let hmac_body = compute_auth_mac(token, AUTH_DOMAIN_BODY, &nonce_brain, &nonce_body);

        let mut auth_resp = Vec::with_capacity(AUTH_NONCE_LEN + AUTH_HMAC_LEN);
        auth_resp.extend_from_slice(&nonce_body);
        auth_resp.extend_from_slice(&hmac_body);
        client
            .write_all(&encode_raw(MSG_AUTH_RESPONSE, 1, &auth_resp).unwrap())
            .await
            .unwrap();

        let (t2, p2) = read_raw_frame(&mut client).await;
        assert_eq!(t2, MSG_WELCOME);
        assert_eq!(p2.len(), 2 + AUTH_HMAC_LEN);
        assert_eq!(&p2[0..2], &[PROTO_VERSION, 16]);
        let brain_mac: [u8; AUTH_HMAC_LEN] = p2[2..].try_into().unwrap();
        assert!(verify_auth_mac(
            token,
            AUTH_DOMAIN_BRAIN,
            &nonce_brain,
            &nonce_body,
            &brain_mac
        ));

        let (t3, p3) = read_raw_frame(&mut client).await;
        assert_eq!(t3, MSG_FACE_STATE);
        assert_eq!(p3, vec![Emotion::Idle as u8, 0, 0, 0]);

        client
            .write_all(&encode_raw(MSG_PING, 0, &7u32.to_le_bytes()).unwrap())
            .await
            .unwrap();
        let (t4, p4) = read_raw_frame(&mut client).await;
        assert_eq!(t4, MSG_PONG);
        assert_eq!(p4, 7u32.to_le_bytes());

        drop(client);
        server_task.await.unwrap().unwrap();
    }

    #[tokio::test]
    async fn rejects_wrong_hmac_before_welcome() {
        let (mut client, server) = tokio::io::duplex(4096);
        let cfg = test_cfg("correct-token", false);
        let (gen_tx, _gen_rx) = watch::channel(0u64);
        let sem = Arc::new(Semaphore::new(2));
        let permit = sem.try_acquire_owned().unwrap();

        let server_task = tokio::spawn(handle_body_stream(
            server,
            "rogue-client".into(),
            cfg,
            Arc::new(gen_tx),
            permit,
        ));

        let hello_payload = vec![PROTO_VERSION, 16, 1, 0b11];
        client
            .write_all(&encode_raw(MSG_HELLO, 0, &hello_payload).unwrap())
            .await
            .unwrap();

        let (t1, p1) = read_raw_frame(&mut client).await;
        assert_eq!(t1, MSG_AUTH_CHALLENGE);
        let nonce_brain: [u8; AUTH_NONCE_LEN] = p1.try_into().unwrap();
        let nonce_body = [0xAAu8; AUTH_NONCE_LEN];
        let bad_hmac = compute_auth_mac("wrong-token", AUTH_DOMAIN_BODY, &nonce_brain, &nonce_body);

        let mut auth_resp = Vec::new();
        auth_resp.extend_from_slice(&nonce_body);
        auth_resp.extend_from_slice(&bad_hmac);
        client
            .write_all(&encode_raw(MSG_AUTH_RESPONSE, 1, &auth_resp).unwrap())
            .await
            .unwrap();

        let res = server_task.await.unwrap();
        assert!(res.is_err());
    }
}
