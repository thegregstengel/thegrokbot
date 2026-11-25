use crate::audio::{pcm16_energy, PcmChunker};
use crate::config::Config;
use crate::protocol::{
    read_body_message, write_brain_message, BodyMessage, BrainMessage, Emotion, PROTO_VERSION,
};
use crate::xai::{XaiCommand, XaiEvent, XaiSession};
use anyhow::Result;
use std::collections::VecDeque;
use std::time::{Duration, Instant};
use tokio::io::{AsyncRead, AsyncWrite};
use tokio::sync::mpsc;
use tokio::time::MissedTickBehavior;
use tracing::{debug, error, info, warn};

/// Number of 20 ms frames sent immediately at utterance start to fill the Body jitter buffer.
const INITIAL_PREFILL_FRAMES: usize = 6;
/// Pacing interval for 20 ms PCM frames (slightly faster than 20.0 ms to prevent I2S underrun).
const FRAME_PACE_US: u64 = 19_800;

/// Bridges one connected ESP-VoCat Body stream (TCP today, USB serial tomorrow)
/// with an xAI Realtime Voice session.
pub async fn handle_body_stream<S>(stream: S, peer_label: String, cfg: Config) -> Result<()>
where
    S: AsyncRead + AsyncWrite + Unpin + Send + 'static,
{
    let (mut reader, mut writer) = tokio::io::split(stream);
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
            maybe_body = rx_in.recv() => {
                let Some(msg) = maybe_body else {
                    break;
                };

                match msg {
                    BodyMessage::Hello {
                        proto_version,
                        sample_rate_khz,
                        channels,
                        caps_flags,
                        device_id,
                    } => {
                        info!(
                            peer = %peer_label,
                            proto_version,
                            sample_rate_khz,
                            channels,
                            caps_flags,
                            %device_id,
                            "ESP-VoCat Body connected"
                        );
                        let _ = tx_body.send(BrainMessage::Welcome {
                            proto_version: PROTO_VERSION,
                            sample_rate_khz: 16,
                        }).await;
                        let _ = send_face(&tx_body, Emotion::Idle, 0).await;
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
                            info!(
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
                            // Server VAD noticed user talking over Grok -> flush Body speaker immediately.
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
                        // Send initial pre-fill frames right away to minimize first-word latency.
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
                        info!(peer = %peer_label, user = %t, "transcript");
                    }
                    Some(XaiEvent::AssistantTranscript(t)) => {
                        info!(peer = %peer_label, grok = %t, "transcript");
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
        encode_raw, FRAME_MAGIC, HEADER_LEN, MSG_FACE_STATE, MSG_HELLO, MSG_PING, MSG_PONG,
        MSG_WELCOME,
    };
    use tokio::io::{AsyncReadExt, AsyncWriteExt};

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

    #[tokio::test]
    async fn body_handshake_and_ping_over_duplex_stream() {
        let (mut client, server) = tokio::io::duplex(4096);
        let cfg = Config {
            xai_api_key: "xai-test".into(),
            listen: "127.0.0.1:0".parse().unwrap(),
            model: "grok-voice-latest".into(),
            voice: "eve".into(),
            instructions: "test".into(),
            reasoning_effort: "none".into(),
            web_search: false,
            x_search: false,
            mdns: false,
            mdns_name: "test".into(),
        };

        let server_task = tokio::spawn(handle_body_stream(server, "test-body".into(), cfg));

        let mut hello_payload = vec![PROTO_VERSION, 16, 1, 0b11];
        hello_payload.extend_from_slice(b"vocat-test");
        let hello = encode_raw(MSG_HELLO, 0, &hello_payload).unwrap();
        client.write_all(&hello).await.unwrap();

        let (t1, p1) = read_raw_frame(&mut client).await;
        assert_eq!(t1, MSG_WELCOME);
        assert_eq!(p1, vec![PROTO_VERSION, 16]);

        let (t2, p2) = read_raw_frame(&mut client).await;
        assert_eq!(t2, MSG_FACE_STATE);
        assert_eq!(p2, vec![Emotion::Idle as u8, 0, 0, 0]);

        let ping = encode_raw(MSG_PING, 0, &7u32.to_le_bytes()).unwrap();
        client.write_all(&ping).await.unwrap();

        let (t3, p3) = read_raw_frame(&mut client).await;
        assert_eq!(t3, MSG_PONG);
        assert_eq!(p3, 7u32.to_le_bytes());

        drop(client);
        server_task.await.unwrap().unwrap();
    }
}
