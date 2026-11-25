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
/// Hold expressive post-speech reactions briefly before relaxing back to Listening.
const EXPRESSION_HOLD_MS: u64 = 1200;
/// Crisp duration of a one-shot wink before relaxing back to an amused smile or Listening.
const WINK_DURATION_MS: u64 = 650;
/// Hold Curious tilt briefly when Grok finishes a reply with a question.
const QUESTION_HOLD_MS: u64 = 2500;

const AUTH_DOMAIN_BODY: &[u8] = b"GB1-BODY";
const AUTH_DOMAIN_BRAIN: &[u8] = b"GB1-BRAIN";

fn contains_word(haystack: &str, word: &str) -> bool {
    let bytes = haystack.as_bytes();
    let wlen = word.len();
    let mut start = 0;
    while let Some(pos) = haystack[start..].find(word) {
        let abs = start + pos;
        let before_ok = abs == 0 || !bytes[abs - 1].is_ascii_alphanumeric();
        let after = abs + wlen;
        let after_ok = after == bytes.len() || !bytes[after].is_ascii_alphanumeric();
        if before_ok && after_ok {
            return true;
        }
        start = abs + 1;
    }
    false
}

/// Maps live assistant transcript cues to subtle GrokBot face expressions.
/// Returns `None` for neutral speech so normal replies stay in `Emotion::Speaking`.
pub fn infer_speaking_emotion(text: &str) -> Option<Emotion> {
    let lower = text.to_lowercase();

    // Playful wink cues (highest priority so punchlines land cleanly).
    const WINK_PHRASES: &[&str] = &[
        "just kidding",
        "don't tell anyone",
        "our little secret",
        "between you and me",
        "between us",
        "if you know what i mean",
        "trust me on this",
        ";)",
        "😉",
    ];
    if WINK_PHRASES.iter().any(|p| lower.contains(p)) || contains_word(&lower, "wink") {
        return Some(Emotion::Wink);
    }

    // Surprise / astonishment cues.
    const SURPRISED_PHRASES: &[&str] = &[
        "no way",
        "wait, what",
        "wait what",
        "plot twist",
        "oh wow",
        "mind blown",
        "didn't see that coming",
        "holy cow",
        "what?!",
        "really?!",
    ];
    const SURPRISED_WORDS: &[&str] = &["whoa", "woah", "unbelievable"];
    if SURPRISED_PHRASES.iter().any(|p| lower.contains(p))
        || SURPRISED_WORDS.iter().any(|w| contains_word(&lower, w))
    {
        return Some(Emotion::Surprised);
    }

    // High-energy celebration / excitement cues.
    const EXCITED_PHRASES: &[&str] = &[
        "let's go",
        "heck yeah",
        "hell yeah",
        "so excited",
        "can't wait",
        "nailed it",
        "awesome!",
        "amazing!",
        "fantastic!",
        "incredible!",
        "brilliant!",
        "love that!",
    ];
    const EXCITED_WORDS: &[&str] = &["woohoo", "congratulations", "congrats"];
    if EXCITED_PHRASES.iter().any(|p| lower.contains(p))
        || EXCITED_WORDS.iter().any(|w| contains_word(&lower, w))
    {
        return Some(Emotion::Excited);
    }

    // Dry skepticism / raised-eyebrow doubt cues.
    const SKEPTICAL_PHRASES: &[&str] = &[
        "are you sure",
        "doubt it",
        "seriously?",
        "really?",
        "bold claim",
        "not so sure",
        "don't know about that",
        "hard to believe",
        "sounds sketchy",
    ];
    const SKEPTICAL_WORDS: &[&str] = &["hmm", "hmmm", "skeptical", "suspicious", "debatable"];
    if SKEPTICAL_PHRASES.iter().any(|p| lower.contains(p))
        || SKEPTICAL_WORDS.iter().any(|w| contains_word(&lower, w))
    {
        return Some(Emotion::Skeptical);
    }

    // Warm amusement / dry humor / chuckle cues.
    const AMUSED_PHRASES: &[&str] = &["good one", "not bad", "gotta admit", "fair point"];
    const AMUSED_WORDS: &[&str] = &[
        "haha",
        "hahaha",
        "hehe",
        "heh",
        "lol",
        "lmao",
        "hilarious",
        "funny",
        "touché",
        "touche",
        "cheeky",
        "ironic",
    ];
    if AMUSED_PHRASES.iter().any(|p| lower.contains(p))
        || AMUSED_WORDS.iter().any(|w| contains_word(&lower, w))
    {
        return Some(Emotion::Amused);
    }

    // Warm cheer / gratitude cues.
    const HAPPY_PHRASES: &[&str] = &[
        "you're welcome",
        "my pleasure",
        "glad to help",
        "happy to help",
        "nice to meet you",
    ];
    if HAPPY_PHRASES.iter().any(|p| lower.contains(p)) {
        return Some(Emotion::Happy);
    }

    // Empathy / apology / caution cues.
    const CONCERNED_PHRASES: &[&str] = &[
        "i'm sorry",
        "so sorry",
        "bad news",
        "that's rough",
        "that's tough",
        "watch out",
        "be careful",
    ];
    const CONCERNED_WORDS: &[&str] = &["unfortunately", "oof", "yikes"];
    if CONCERNED_PHRASES.iter().any(|p| lower.contains(p))
        || CONCERNED_WORDS.iter().any(|w| contains_word(&lower, w))
    {
        return Some(Emotion::Concerned);
    }

    None
}

/// Detects explicit user requests for GrokBot to make a specific facial expression
/// (for example: "wink at me", "look skeptical", "act surprised", "give me a smirk").
pub fn infer_user_expression_request(text: &str) -> Option<Emotion> {
    let lower = text.to_lowercase();

    // Direct gesture phrases that clearly command an expression on their own.
    if contains_word(&lower, "wink") || contains_word(&lower, "winking") {
        return Some(Emotion::Wink);
    }
    if contains_word(&lower, "smirk") {
        return Some(Emotion::Amused);
    }
    if lower.contains("side eye")
        || lower.contains("side-eye")
        || lower.contains("raise an eyebrow")
        || lower.contains("raise your eyebrow")
    {
        return Some(Emotion::Skeptical);
    }
    if lower.contains("smile for me")
        || lower.contains("smile at me")
        || lower.contains("give me a smile")
    {
        return Some(Emotion::Happy);
    }

    const REQUEST_CUES: &[&str] = &[
        "look ",
        "act ",
        "be ",
        "face",
        "expression",
        "show me",
        "give me",
        "can you",
        "do a ",
        "do your ",
        "make a ",
        "try ",
    ];
    if !REQUEST_CUES.iter().any(|cue| lower.contains(cue)) {
        return None;
    }

    if contains_word(&lower, "surprised") || contains_word(&lower, "shocked") {
        return Some(Emotion::Surprised);
    }
    if contains_word(&lower, "skeptical") || contains_word(&lower, "suspicious") {
        return Some(Emotion::Skeptical);
    }
    if contains_word(&lower, "excited") {
        return Some(Emotion::Excited);
    }
    if contains_word(&lower, "amused") || contains_word(&lower, "smug") {
        return Some(Emotion::Amused);
    }
    if contains_word(&lower, "happy") || contains_word(&lower, "smile") {
        return Some(Emotion::Happy);
    }
    if contains_word(&lower, "sleepy") || contains_word(&lower, "tired") {
        return Some(Emotion::Sleepy);
    }
    if contains_word(&lower, "curious") {
        return Some(Emotion::Curious);
    }
    if contains_word(&lower, "concerned")
        || contains_word(&lower, "worried")
        || contains_word(&lower, "sad")
    {
        return Some(Emotion::Concerned);
    }

    None
}

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
    let mut response_done_seen = false;
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
    let mut turn_transcript = String::new();
    let mut speaking_emotion = Emotion::Speaking;
    let mut requested_emotion: Option<Emotion> = None;
    let mut turn_locked_emotion = false;
    let mut pending_expression: Option<(Emotion, Instant)> = None;
    let mut wink_release_at: Option<Instant> = None;
    let mut wink_used_this_turn = false;
    let mut post_speech_settle_at: Option<Instant> = None;

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
                        response_done_seen = false;
                        prefill_remaining = 0;
                        turn_transcript.clear();
                        speaking_emotion = Emotion::Speaking;
                        requested_emotion = None;
                        turn_locked_emotion = false;
                        pending_expression = None;
                        wink_release_at = None;
                        wink_used_this_turn = false;
                        post_speech_settle_at = None;
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
                        response_done_seen = false;
                        prefill_remaining = 0;
                        turn_transcript.clear();
                        speaking_emotion = Emotion::Speaking;
                        requested_emotion = None;
                        turn_locked_emotion = false;
                        pending_expression = None;
                        wink_release_at = None;
                        wink_used_this_turn = false;
                        post_speech_settle_at = None;
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
                        response_done_seen = false;
                        prefill_remaining = 0;
                        turn_transcript.clear();
                        speaking_emotion = Emotion::Speaking;
                        requested_emotion = None;
                        turn_locked_emotion = false;
                        pending_expression = None;
                        wink_release_at = None;
                        wink_used_this_turn = false;
                        post_speech_settle_at = None;
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
                        requested_emotion = None;
                        turn_locked_emotion = false;
                        post_speech_settle_at = None;
                        pending_expression = None;
                        wink_release_at = None;
                        if speaking {
                            speaking = false;
                            response_done_pending = false;
                            response_done_seen = false;
                            prefill_remaining = 0;
                            turn_transcript.clear();
                            speaking_emotion = Emotion::Speaking;
                            wink_used_this_turn = false;
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
                        response_done_seen = false;
                        turn_transcript.clear();
                        speaking_emotion = Emotion::Speaking;
                        turn_locked_emotion = false;
                        pending_expression = None;
                        wink_release_at = None;
                        wink_used_this_turn = false;
                        post_speech_settle_at = None;
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
                            if let Some(req) = requested_emotion.take() {
                                speaking_emotion = req;
                                turn_locked_emotion = true;
                                pending_expression = None;
                                if req == Emotion::Wink {
                                    wink_used_this_turn = true;
                                    wink_release_at = Some(
                                        Instant::now() + Duration::from_millis(WINK_DURATION_MS),
                                    );
                                }
                                info!(peer = %peer_label, ?req, "applying user-requested expression");
                            }
                            info!(peer = %peer_label, bytes = pcm.len(), "xAI audio playback started");
                            let _ = send_face(&tx_body, speaking_emotion, 96).await;
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
                        if response_done_seen {
                            continue;
                        }
                        response_done_seen = true;
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
                            if let Some(req) = requested_emotion.take() {
                                speaking_emotion = req;
                            } else if let Some((emo, _)) = pending_expression.take() {
                                speaking_emotion = emo;
                            }
                            wink_release_at = None;
                            if awake {
                                post_speech_settle_at = settle_post_speech_face(
                                    &tx_body,
                                    speaking_emotion,
                                    &turn_transcript,
                                )
                                .await;
                            }
                        } else {
                            response_done_pending = true;
                        }
                    }
                    Some(XaiEvent::UserTranscript(t)) => {
                        if let Some(req) = infer_user_expression_request(&t) {
                            if awake && speaking {
                                speaking_emotion = req;
                                turn_locked_emotion = true;
                                pending_expression = None;
                                if req == Emotion::Wink {
                                    wink_used_this_turn = true;
                                    wink_release_at = Some(
                                        Instant::now() + Duration::from_millis(WINK_DURATION_MS),
                                    );
                                }
                                info!(peer = %peer_label, ?req, "applying user-requested expression live");
                                let _ = send_face(&tx_body, speaking_emotion, 96).await;
                            } else {
                                requested_emotion = Some(req);
                                debug!(peer = %peer_label, ?req, "queued user-requested expression for reply");
                            }
                        }
                        if cfg.log_transcripts {
                            info!(peer = %peer_label, user = %t, "transcript");
                        } else {
                            debug!(peer = %peer_label, chars = t.len(), "user transcript received");
                        }
                    }
                    Some(XaiEvent::AssistantTranscriptDelta(delta)) => {
                        turn_transcript.push_str(&delta);
                        if turn_locked_emotion {
                            continue;
                        }
                        if let Some(detected) = infer_speaking_emotion(&turn_transcript) {
                            if detected == Emotion::Wink && wink_used_this_turn {
                                continue;
                            }
                            let already_scheduled = pending_expression
                                .as_ref()
                                .map(|(e, _)| *e == detected)
                                .unwrap_or(false);
                            let should_schedule = !already_scheduled
                                && ((speaking_emotion == Emotion::Speaking
                                    && pending_expression.is_none())
                                    || (detected == Emotion::Wink
                                        && speaking_emotion != Emotion::Wink));
                            if should_schedule {
                                // Sync expression timing with buffered playback frames so cues
                                // land near the spoken phrase rather than several seconds early.
                                let delay_ms = (play_queue
                                    .len()
                                    .saturating_sub(INITIAL_PREFILL_FRAMES)
                                    as u64
                                    * 18)
                                    .min(8000);
                                if delay_ms <= 120 {
                                    speaking_emotion = detected;
                                    if detected == Emotion::Wink {
                                        wink_used_this_turn = true;
                                        wink_release_at = Some(
                                            Instant::now()
                                                + Duration::from_millis(WINK_DURATION_MS),
                                        );
                                    }
                                    debug!(peer = %peer_label, ?speaking_emotion, "live expression applied");
                                    if awake && speaking {
                                        let _ = send_face(&tx_body, speaking_emotion, 96).await;
                                    }
                                } else {
                                    pending_expression = Some((
                                        detected,
                                        Instant::now() + Duration::from_millis(delay_ms),
                                    ));
                                    debug!(peer = %peer_label, ?detected, delay_ms, "scheduled playback-synced expression");
                                }
                            }
                        }
                    }
                    Some(XaiEvent::AssistantTranscript(t)) => {
                        if turn_transcript.is_empty() {
                            turn_transcript = t.clone();
                        }
                        if !turn_locked_emotion
                            && speaking_emotion == Emotion::Speaking
                            && pending_expression.is_none()
                        {
                            if let Some(detected) = infer_speaking_emotion(&t) {
                                if !(detected == Emotion::Wink && wink_used_this_turn) {
                                    speaking_emotion = detected;
                                    if detected == Emotion::Wink {
                                        wink_used_this_turn = true;
                                        wink_release_at = Some(
                                            Instant::now()
                                                + Duration::from_millis(WINK_DURATION_MS),
                                        );
                                    }
                                    if awake && speaking {
                                        let _ = send_face(&tx_body, speaking_emotion, 96).await;
                                    }
                                }
                            }
                        }
                        if cfg.log_transcripts {
                            info!(peer = %peer_label, grok = %t, "transcript");
                        } else {
                            debug!(peer = %peer_label, chars = t.len(), "assistant transcript received");
                        }
                    }
                    Some(XaiEvent::Error(err)) => {
                        error!(peer = %peer_label, %err, "xAI server error");
                        requested_emotion = None;
                        turn_locked_emotion = false;
                        pending_expression = None;
                        wink_release_at = None;
                        post_speech_settle_at = None;
                        let _ = send_face(&tx_body, Emotion::Concerned, 0).await;
                    }
                    Some(XaiEvent::Disconnected) | None => {
                        warn!(peer = %peer_label, "xAI session closed; will auto-resume on next wake");
                        xai = None;
                        speaking = false;
                        response_done_pending = false;
                        response_done_seen = false;
                        prefill_remaining = 0;
                        turn_transcript.clear();
                        speaking_emotion = Emotion::Speaking;
                        requested_emotion = None;
                        turn_locked_emotion = false;
                        pending_expression = None;
                        wink_release_at = None;
                        wink_used_this_turn = false;
                        post_speech_settle_at = None;
                        chunker.clear();
                        play_queue.clear();
                        if awake {
                            let _ = send_face(&tx_body, Emotion::Concerned, 0).await;
                            awake = false;
                        }
                    }
                }
            }

            _ = async {
                match pending_expression {
                    Some((_, deadline)) => {
                        tokio::time::sleep_until(tokio::time::Instant::from_std(deadline)).await
                    }
                    None => std::future::pending().await,
                }
            } => {
                if let Some((emo, _)) = pending_expression.take() {
                    if awake && speaking {
                        speaking_emotion = emo;
                        if emo == Emotion::Wink {
                            wink_used_this_turn = true;
                            wink_release_at =
                                Some(Instant::now() + Duration::from_millis(WINK_DURATION_MS));
                        }
                        debug!(peer = %peer_label, ?speaking_emotion, "playback-synced expression fired");
                        let _ = send_face(&tx_body, speaking_emotion, 96).await;
                    }
                }
            }

            _ = async {
                match wink_release_at {
                    Some(deadline) => {
                        tokio::time::sleep_until(tokio::time::Instant::from_std(deadline)).await
                    }
                    None => std::future::pending().await,
                }
            } => {
                wink_release_at = None;
                if speaking_emotion == Emotion::Wink {
                    speaking_emotion = Emotion::Amused;
                    if awake && speaking {
                        let _ = send_face(&tx_body, speaking_emotion, 80).await;
                    }
                }
            }

            _ = async {
                match post_speech_settle_at {
                    Some(deadline) => {
                        tokio::time::sleep_until(tokio::time::Instant::from_std(deadline)).await
                    }
                    None => std::future::pending().await,
                }
            } => {
                post_speech_settle_at = None;
                if awake && !speaking {
                    let _ = send_face(&tx_body, Emotion::Listening, 0).await;
                }
            }

            _ = pace_tick.tick(), if !play_queue.is_empty() => {
                if let Some(frame) = play_queue.pop_front() {
                    if last_face_energy_at.elapsed() >= Duration::from_millis(50) {
                        let energy = pcm16_energy(&frame);
                        let _ = send_face(&tx_body, speaking_emotion, energy).await;
                        last_face_energy_at = Instant::now();
                    }
                    let _ = tx_body.send(BrainMessage::AudioDown(frame)).await;
                }
                if play_queue.is_empty() && response_done_pending {
                    speaking = false;
                    response_done_pending = false;
                    if let Some((emo, _)) = pending_expression.take() {
                        speaking_emotion = emo;
                    }
                    wink_release_at = None;
                    if awake {
                        post_speech_settle_at = settle_post_speech_face(
                            &tx_body,
                            speaking_emotion,
                            &turn_transcript,
                        )
                        .await;
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

async fn settle_post_speech_face(
    tx: &mpsc::Sender<BrainMessage>,
    speaking_emotion: Emotion,
    turn_transcript: &str,
) -> Option<Instant> {
    if speaking_emotion == Emotion::Wink {
        let _ = send_face(tx, Emotion::Wink, 0).await;
        return Some(Instant::now() + Duration::from_millis(WINK_DURATION_MS));
    }
    if speaking_emotion != Emotion::Speaking {
        let _ = send_face(tx, speaking_emotion, 0).await;
        return Some(Instant::now() + Duration::from_millis(EXPRESSION_HOLD_MS));
    }
    if turn_transcript.trim_end().ends_with('?') {
        let _ = send_face(tx, Emotion::Curious, 0).await;
        return Some(Instant::now() + Duration::from_millis(QUESTION_HOLD_MS));
    }
    let _ = send_face(tx, Emotion::Listening, 0).await;
    None
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

    #[test]
    fn infer_speaking_emotion_detects_new_expressions() {
        assert_eq!(
            infer_speaking_emotion("Just kidding, don't tell anyone."),
            Some(Emotion::Wink)
        );
        assert_eq!(
            infer_speaking_emotion("Whoa, no way! I didn't see that coming."),
            Some(Emotion::Surprised)
        );
        assert_eq!(
            infer_speaking_emotion("Let's go! You nailed it!"),
            Some(Emotion::Excited)
        );
        assert_eq!(
            infer_speaking_emotion("Hmm, are you sure about that?"),
            Some(Emotion::Skeptical)
        );
        assert_eq!(
            infer_speaking_emotion("Haha, fair point, that's a good one."),
            Some(Emotion::Amused)
        );
        assert_eq!(
            infer_speaking_emotion("You're welcome, happy to help."),
            Some(Emotion::Happy)
        );
        assert_eq!(
            infer_speaking_emotion("I'm sorry, unfortunately that failed."),
            Some(Emotion::Concerned)
        );
        // Neutral text and substring false positives stay in default Speaking.
        assert_eq!(
            infer_speaking_emotion("The stars twinkle quietly above the horizon."),
            None
        );
        assert_eq!(
            infer_speaking_emotion("It is currently 68 degrees and sunny."),
            None
        );
    }

    #[test]
    fn infer_user_expression_request_matches_commands_without_false_positives() {
        assert_eq!(
            infer_user_expression_request("Wink at me!"),
            Some(Emotion::Wink)
        );
        assert_eq!(
            infer_user_expression_request("Can you give me a skeptical look?"),
            Some(Emotion::Skeptical)
        );
        assert_eq!(
            infer_user_expression_request("Give me the side eye."),
            Some(Emotion::Skeptical)
        );
        assert_eq!(
            infer_user_expression_request("Act surprised!"),
            Some(Emotion::Surprised)
        );
        assert_eq!(
            infer_user_expression_request("Show me your excited face."),
            Some(Emotion::Excited)
        );
        assert_eq!(
            infer_user_expression_request("Give me a smirk."),
            Some(Emotion::Amused)
        );
        assert_eq!(
            infer_user_expression_request("Smile for me!"),
            Some(Emotion::Happy)
        );
        assert_eq!(
            infer_user_expression_request("Look sleepy."),
            Some(Emotion::Sleepy)
        );
        // Normal conversation mentioning emotions should not hijack the face.
        assert_eq!(
            infer_user_expression_request("I am surprised it rained today."),
            None
        );
        assert_eq!(
            infer_user_expression_request("Are you excited for Friday?"),
            None
        );
    }
}
