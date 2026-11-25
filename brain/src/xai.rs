use crate::audio::SAMPLE_RATE_HZ;
use crate::config::Config;
use anyhow::{anyhow, Context, Result};
use base64::Engine;
use futures_util::{SinkExt, StreamExt};
use http::Request;
use serde_json::{json, Value};
use tokio::sync::mpsc;
use tokio_tungstenite::{connect_async, tungstenite::Message};
use tracing::{debug, error, info, warn};

#[derive(Debug)]
pub enum XaiCommand {
    SendPcm(Vec<u8>),
    CancelResponse,
    Shutdown,
}

#[derive(Debug)]
pub enum XaiEvent {
    SessionReady { conversation_id: Option<String> },
    SpeechStarted,
    SpeechStopped,
    ResponseStarted,
    AudioDelta(Vec<u8>),
    ResponseDone,
    UserTranscript(String),
    AssistantTranscriptDelta(String),
    AssistantTranscript(String),
    Error(String),
    Disconnected,
}

pub struct XaiSession {
    pub cmd_tx: mpsc::Sender<XaiCommand>,
    pub evt_rx: mpsc::Receiver<XaiEvent>,
}

impl XaiSession {
    pub async fn connect(cfg: Config, resume_conversation_id: Option<String>) -> Result<Self> {
        let mut url = format!("wss://api.x.ai/v1/realtime?model={}", cfg.model);
        if let Some(ref conv_id) = resume_conversation_id {
            url.push_str("&conversation_id=");
            url.push_str(conv_id);
        }

        let request = Request::builder()
            .uri(&url)
            .header("Host", "api.x.ai")
            .header("Authorization", format!("Bearer {}", cfg.xai_api_key.trim()))
            .header("Connection", "Upgrade")
            .header("Upgrade", "websocket")
            .header("Sec-WebSocket-Version", "13")
            .header(
                "Sec-WebSocket-Key",
                tokio_tungstenite::tungstenite::handshake::client::generate_key(),
            )
            .body(())
            .context("failed to build xAI WebSocket request")?;

        info!(%url, "connecting to xAI realtime voice");
        let (ws_stream, resp) = connect_async(request)
            .await
            .map_err(|e| anyhow!("xAI WebSocket connect failed: {e}"))?;
        info!(status = %resp.status(), "xAI WebSocket connected");

        let (mut ws_write, mut ws_read) = ws_stream.split();

        let update = build_session_update(&cfg);
        ws_write
            .send(Message::Text(update.to_string().into()))
            .await
            .context("failed to send session.update")?;

        let (cmd_tx, mut cmd_rx) = mpsc::channel::<XaiCommand>(128);
        let (evt_tx, evt_rx) = mpsc::channel::<XaiEvent>(256);

        tokio::spawn(async move {
            let mut conversation_id = resume_conversation_id;

            loop {
                tokio::select! {
                    maybe_cmd = cmd_rx.recv() => {
                        match maybe_cmd {
                            Some(XaiCommand::SendPcm(pcm)) => {
                                if let Err(e) = ws_write.send(Message::Binary(pcm.into())).await {
                                    warn!("xAI audio send failed: {e}");
                                    break;
                                }
                            }
                            Some(XaiCommand::CancelResponse) => {
                                let cancel = json!({ "type": "response.cancel" });
                                let clear = json!({ "type": "input_audio_buffer.clear" });
                                let _ = ws_write.send(Message::Text(cancel.to_string().into())).await;
                                let _ = ws_write.send(Message::Text(clear.to_string().into())).await;
                            }
                            Some(XaiCommand::Shutdown) | None => {
                                let _ = ws_write.close().await;
                                break;
                            }
                        }
                    }

                    maybe_msg = ws_read.next() => {
                        match maybe_msg {
                            Some(Ok(Message::Binary(bin))) => {
                                if evt_tx.send(XaiEvent::AudioDelta(bin.to_vec())).await.is_err() {
                                    break;
                                }
                            }
                            Some(Ok(Message::Text(text))) => {
                                if let Some(evt) = parse_xai_json(&text, &mut conversation_id) {
                                    if evt_tx.send(evt).await.is_err() {
                                        break;
                                    }
                                }
                            }
                            Some(Ok(Message::Ping(payload))) => {
                                let _ = ws_write.send(Message::Pong(payload)).await;
                            }
                            Some(Ok(Message::Close(frame))) => {
                                info!(?frame, "xAI WebSocket closed by server");
                                break;
                            }
                            Some(Ok(_)) => {}
                            Some(Err(e)) => {
                                error!("xAI WebSocket read error: {e}");
                                let _ = evt_tx.send(XaiEvent::Error(e.to_string())).await;
                                break;
                            }
                            None => break,
                        }
                    }
                }
            }

            let _ = evt_tx.send(XaiEvent::Disconnected).await;
        });

        Ok(Self { cmd_tx, evt_rx })
    }
}

fn build_session_update(cfg: &Config) -> Value {
    let mut tools = Vec::new();
    if cfg.web_search {
        tools.push(json!({ "type": "web_search" }));
    }
    if cfg.x_search {
        tools.push(json!({ "type": "x_search" }));
    }

    let mut session = json!({
        "voice": cfg.voice,
        "instructions": cfg.instructions,
        "turn_detection": {
            "type": "server_vad",
            "threshold": 0.45,
            "silence_duration_ms": 500,
            "prefix_padding_ms": 300
        },
        "resumption": { "enabled": true },
        "audio": {
            "input": {
                "format": { "type": "audio/pcm", "rate": SAMPLE_RATE_HZ },
                "transport": "binary"
            },
            "output": {
                "format": { "type": "audio/pcm", "rate": SAMPLE_RATE_HZ },
                "transport": "binary"
            }
        }
    });

    if !cfg.reasoning_effort.is_empty() {
        session["reasoning"] = json!({ "effort": cfg.reasoning_effort });
    }
    if !tools.is_empty() {
        session["tools"] = Value::Array(tools);
    }

    json!({
        "type": "session.update",
        "session": session
    })
}

fn parse_xai_json(text: &str, conversation_id: &mut Option<String>) -> Option<XaiEvent> {
    let root: Value = match serde_json::from_str(text) {
        Ok(v) => v,
        Err(e) => {
            warn!("failed to parse xAI JSON: {e}");
            return None;
        }
    };

    let event_type = root.get("type")?.as_str()?;
    match event_type {
        "conversation.created" => {
            if let Some(id) = root
                .get("conversation")
                .and_then(|c| c.get("id"))
                .and_then(|id| id.as_str())
            {
                info!(conversation_id = id, "xAI conversation created");
                *conversation_id = Some(id.to_string());
            }
            None
        }
        "session.created" | "session.updated" => {
            info!(event_type, "xAI session ready");
            Some(XaiEvent::SessionReady {
                conversation_id: conversation_id.clone(),
            })
        }
        "input_audio_buffer.speech_started" => Some(XaiEvent::SpeechStarted),
        "input_audio_buffer.speech_stopped" => Some(XaiEvent::SpeechStopped),
        "response.created" => Some(XaiEvent::ResponseStarted),
        // Fallback if server ever sends JSON base64 audio before switching to binary transport.
        "response.output_audio.delta" | "response.audio.delta" => {
            let b64 = root
                .get("delta")
                .or_else(|| root.get("audio"))
                .and_then(|v| v.as_str())?;
            match base64::engine::general_purpose::STANDARD.decode(b64) {
                Ok(pcm) => Some(XaiEvent::AudioDelta(pcm)),
                Err(e) => {
                    warn!("base64 decode error on fallback delta: {e}");
                    None
                }
            }
        }
        "response.output_audio.done" | "response.audio.done" | "response.done" => {
            Some(XaiEvent::ResponseDone)
        }
        "conversation.item.input_audio_transcription.completed"
        | "conversation.item.input_audio_transcription.done" => root
            .get("transcript")
            .and_then(|v| v.as_str())
            .map(|s| XaiEvent::UserTranscript(s.to_string())),
        "response.output_audio_transcript.delta" | "response.audio_transcript.delta" => root
            .get("delta")
            .and_then(|v| v.as_str())
            .map(|s| XaiEvent::AssistantTranscriptDelta(s.to_string())),
        "response.output_audio_transcript.done" | "response.audio_transcript.done" => root
            .get("transcript")
            .and_then(|v| v.as_str())
            .map(|s| XaiEvent::AssistantTranscript(s.to_string())),
        "error" => {
            let msg = root
                .get("error")
                .and_then(|e| e.get("message"))
                .and_then(|m| m.as_str())
                .unwrap_or("unknown xAI error");
            Some(XaiEvent::Error(msg.to_string()))
        }
        other => {
            debug!(event_type = other, "ignoring xAI event");
            None
        }
    }
}
