use clap::Parser;
use std::net::SocketAddr;

const DEFAULT_INSTRUCTIONS: &str = "You are GrokBot, a witty, warm, concise physical companion living on a small round desk device with expressive eyes. Keep spoken responses brief, natural, and conversational unless asked for detail.";

#[derive(Debug, Clone, Parser)]
#[command(name = "grokbot-brain", about = "Companion Brain daemon for Physical GrokBot")]
pub struct Config {
    /// xAI API key for wss://api.x.ai/v1/realtime
    #[arg(long, env = "XAI_API_KEY")]
    pub xai_api_key: String,

    /// Address and port to listen on for ESP-VoCat Body connections
    #[arg(long, env = "GROKBOT_LISTEN", default_value = "0.0.0.0:8765")]
    pub listen: SocketAddr,

    /// Shared pairing token for mutual HMAC-SHA256 authentication with the Body
    #[arg(long, env = "GROKBOT_AUTH_TOKEN", default_value = "")]
    pub auth_token: String,

    /// Explicitly allow unauthenticated Body connections (not recommended on shared LANs)
    #[arg(long, env = "GROKBOT_ALLOW_UNAUTHENTICATED", default_value_t = false)]
    pub allow_unauthenticated: bool,

    /// Log full user and assistant voice transcripts at INFO level
    #[arg(long, env = "GROKBOT_LOG_TRANSCRIPTS", default_value_t = false)]
    pub log_transcripts: bool,

    /// xAI realtime voice model
    #[arg(long, env = "GROKBOT_XAI_MODEL", default_value = "grok-voice-latest")]
    pub model: String,

    /// xAI voice name (for example: eve, ara, rex, sal, leo)
    #[arg(long, env = "GROKBOT_XAI_VOICE", default_value = "eve")]
    pub voice: String,

    /// System instructions for GrokBot
    #[arg(long, env = "GROKBOT_INSTRUCTIONS", default_value = DEFAULT_INSTRUCTIONS)]
    pub instructions: String,

    /// Reasoning effort: "none" (lowest latency) or "high"
    #[arg(long, env = "GROKBOT_REASONING_EFFORT", default_value = "none")]
    pub reasoning_effort: String,

    /// Enable xAI built-in web_search tool
    #[arg(long, env = "GROKBOT_WEB_SEARCH", default_value_t = false)]
    pub web_search: bool,

    /// Enable xAI built-in x_search tool
    #[arg(long, env = "GROKBOT_X_SEARCH", default_value_t = false)]
    pub x_search: bool,

    /// Advertise _grokbot._tcp.local via mDNS so ESP-VoCat finds this host automatically
    #[arg(long, env = "GROKBOT_MDNS", default_value_t = true)]
    pub mdns: bool,

    /// mDNS instance name
    #[arg(long, env = "GROKBOT_MDNS_NAME", default_value = "grokbot-brain")]
    pub mdns_name: String,
}
