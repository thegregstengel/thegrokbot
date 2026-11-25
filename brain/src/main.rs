mod audio;
mod config;
mod protocol;
mod session;
mod xai;

use anyhow::{Context, Result};
use clap::Parser;
use config::Config;
use mdns_sd::{ServiceDaemon, ServiceInfo};
use std::sync::Arc;
use tokio::net::TcpListener;
use tokio::sync::{watch, Semaphore};
use tracing::{error, info, warn};

const MDNS_SERVICE_TYPE: &str = "_grokbot._tcp.local.";
/// Caps concurrent in-flight handshakes to prevent unauthenticated connection floods.
const MAX_CONCURRENT_HANDSHAKES: usize = 4;

#[tokio::main]
async fn main() -> Result<()> {
    let _ = rustls::crypto::ring::default_provider().install_default();

    tracing_subscriber::fmt()
        .with_env_filter(
            tracing_subscriber::EnvFilter::try_from_default_env()
                .unwrap_or_else(|_| "grokbot_brain=info".into()),
        )
        .init();

    let cfg = Config::parse();
    if cfg.xai_api_key.trim().is_empty() {
        anyhow::bail!("XAI_API_KEY is empty. Pass --xai-api-key or set XAI_API_KEY.");
    }

    let auth_enabled = !cfg.auth_token.trim().is_empty();
    if !auth_enabled {
        if !cfg.allow_unauthenticated {
            anyhow::bail!(
                "GROKBOT_AUTH_TOKEN is empty. Set a shared pairing token (or pass \
                 --allow-unauthenticated on a strictly private LAN)."
            );
        }
        warn!(
            "Running WITHOUT Body authentication (--allow-unauthenticated). \
             Any device on this network can use your XAI_API_KEY."
        );
    }

    let _mdns_guard = if cfg.mdns {
        match advertise_mdns(&cfg.mdns_name, cfg.listen.port()) {
            Ok(daemon) => Some(daemon),
            Err(e) => {
                warn!("mDNS advertisement unavailable ({e}); ESP-VoCat can still connect by IP");
                None
            }
        }
    } else {
        None
    };

    let listener = TcpListener::bind(cfg.listen)
        .await
        .with_context(|| format!("failed to bind {}", cfg.listen))?;

    info!(
        listen = %cfg.listen,
        model = %cfg.model,
        voice = %cfg.voice,
        auth = auth_enabled,
        log_transcripts = cfg.log_transcripts,
        "GrokBot Brain listening for ESP-VoCat connections"
    );

    let handshake_sem = Arc::new(Semaphore::new(MAX_CONCURRENT_HANDSHAKES));
    let (active_gen_tx, _active_gen_rx) = watch::channel::<u64>(0);
    let active_gen_tx = Arc::new(active_gen_tx);

    loop {
        let (socket, peer) = listener.accept().await?;
        let Ok(permit) = handshake_sem.clone().try_acquire_owned() else {
            warn!(peer = %peer, "rejecting connection: too many concurrent handshakes");
            drop(socket);
            continue;
        };

        let _ = socket.set_nodelay(true);
        let peer_label = peer.to_string();
        let cfg_clone = cfg.clone();
        let gen_tx = active_gen_tx.clone();
        info!(peer = %peer_label, "accepted TCP connection");

        tokio::spawn(async move {
            if let Err(e) =
                session::handle_body_stream(socket, peer_label.clone(), cfg_clone, gen_tx, permit)
                    .await
            {
                error!(peer = %peer_label, "session error: {e}");
            }
        });
    }
}

fn advertise_mdns(instance_name: &str, port: u16) -> Result<ServiceDaemon> {
    let mdns = ServiceDaemon::new().context("failed to start mDNS daemon")?;
    let host_name = format!("{instance_name}.local.");
    let service = ServiceInfo::new(
        MDNS_SERVICE_TYPE,
        instance_name,
        &host_name,
        "",
        port,
        None,
    )?
    .enable_addr_auto();

    mdns.register(service)
        .context("failed to register _grokbot._tcp.local. service")?;
    info!(
        service = MDNS_SERVICE_TYPE,
        instance = instance_name,
        port,
        "advertising Brain via mDNS"
    );
    Ok(mdns)
}
