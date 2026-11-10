//! The headless `send --to` mode: discover the destination, send the files
//! or the text message, exit with the transfer result. There is no keyboard and no overlay — only
//! the network events a send needs are handled, and incoming transfer
//! requests are declined.

use super::sending::{self, Payload, SendState};
use super::status;
use super::target::TargetSelector;
use super::{AppEvent, Network, discovery, spawn_staged_discovery, start_network, stop_network};
use crate::sanitize;
use crate::storage::Repository;
use crate::ui::{Category, Ui};
use crate::util;
use localsend::discovery::{DiscoveryEvent, DiscoveryHandle};
use localsend::http::server::common::save::FileUploadTarget;
use localsend::http::server::v2::{PrepareUploadDecisionV2, ServerEventV2, SessionEndReasonV2};
use std::collections::{HashMap, HashSet};
use std::net::IpAddr;
use std::path::PathBuf;
use std::sync::Arc;
use std::sync::atomic::Ordering;
use std::time::Duration;
use tokio::sync::{mpsc, oneshot};

pub(super) async fn run(
    mut storage: Repository,
    target: TargetSelector,
    payload: Payload,
) -> anyhow::Result<()> {
    let mut ui = Ui::new();
    let identity = storage.identity.clone();
    let (events_tx, mut events_rx) = mpsc::channel::<AppEvent>(64);
    let Network {
        server,
        server_stop_tx,
        server_tx: _,
        mut server_rx,
        discovery,
        mut discovery_rx,
        discovery_stop_tx,
    } = start_network(&identity).await?;

    // Probe the destination's address directly when `--to` is an IP.
    let mut known_channels = storage.paired.known_http_channels();
    if let Some(channel) = target.direct_channel()
        && !known_channels.contains(&channel)
    {
        known_channels.insert(0, channel);
    }
    spawn_staged_discovery(
        discovery.clone(),
        events_tx.clone(),
        known_channels,
        identity.port,
    );
    ui.log_plain(&format!("Discovering destination {target}..."));

    let mut tick = tokio::time::interval(Duration::from_millis(250));
    tick.set_missed_tick_behavior(tokio::time::MissedTickBehavior::Skip);

    let mut send: Option<SendState> = None;
    let result = loop {
        let event = tokio::select! {
            Some(event) = events_rx.recv() => event,
            Some(event) = server_rx.recv() => AppEvent::Server(event),
            Some(event) = discovery_rx.recv() => AppEvent::Discovery(event),
            _ = tick.tick() => AppEvent::Tick,
        };
        match event {
            AppEvent::Server(event) => {
                handle_server_event(&mut ui, &storage, &discovery, &mut send, event)
            }
            AppEvent::Discovery(event) => {
                handle_discovery(&mut ui, &mut storage, &discovery, event)
            }
            AppEvent::Tick => render_progress(&mut ui, &mut send),
            AppEvent::Log { category, text } => ui.log(category, &text),
            AppEvent::DiscoveryFinished => {
                match start_send(&mut ui, &storage, &discovery, &target, &payload, &events_tx) {
                    Ok(state) => send = Some(state),
                    Err(reason) => break Err(anyhow::anyhow!(reason)),
                }
            }
            AppEvent::SendSessionStarted {
                session_id,
                accepted_bytes,
            } => {
                if let Some(send) = &mut send {
                    send.session_id = Some(session_id);
                    send.total_bytes = accepted_bytes;
                }
            }
            AppEvent::SendEnded { success } => {
                break match success {
                    true => Ok(()),
                    false => Err(anyhow::anyhow!("Transfer failed")),
                };
            }
            // No keyboard, no accepted receive sessions and no web links in
            // this mode.
            AppEvent::Key(_) | AppEvent::ReceiveFileResult { .. } | AppEvent::Web(_) => {}
        }
    };
    ui.set_status(None);
    stop_network(&server, Some(server_stop_tx), &discovery, discovery_stop_tx).await;
    result
}

/// Resolves the destination among the discovered devices and spawns the send
/// task towards it.
fn start_send(
    ui: &mut Ui,
    storage: &Repository,
    discovery: &DiscoveryHandle,
    target: &TargetSelector,
    payload: &Payload,
    events_tx: &mpsc::Sender<AppEvent>,
) -> Result<SendState, String> {
    let fingerprint = target.resolve(&discovery.devices())?;
    let device = discovery
        .device_by_fingerprint(&fingerprint)
        .ok_or_else(|| format!("Destination {target} was not discovered"))?;
    match payload {
        Payload::Files(paths) => sending::spawn_send(
            ui,
            storage.identity.clone(),
            device,
            paths.clone(),
            events_tx.clone(),
        ),
        Payload::Text(text) => sending::spawn_send_text(
            ui,
            storage.identity.clone(),
            device,
            text.clone(),
            events_tx.clone(),
        ),
    }
}

fn handle_server_event(
    ui: &mut Ui,
    storage: &Repository,
    discovery: &Arc<DiscoveryHandle>,
    send: &mut Option<SendState>,
    event: ServerEventV2,
) {
    match event {
        // Peers answer the startup announcement by registering here; this is
        // how the destination usually enters the device store.
        ServerEventV2::Register { ip, info } => {
            discovery::device_confirmed(
                discovery,
                &storage.identity.fingerprint,
                ip.to_string(),
                info,
            );
        }
        ServerEventV2::PrepareUpload {
            info, decision_tx, ..
        } => {
            // Receiving needs the interactive Y/N prompt; decline instead of
            // leaving the sender hanging.
            let _ = decision_tx.send(PrepareUploadDecisionV2::Decline);
            ui.log(
                Category::Receive,
                &format!(
                    "{}: Declined the incoming request (busy sending)",
                    sanitize::single_line(&info.alias)
                ),
            );
        }
        ServerEventV2::CancelReceived { ip, session_id } => {
            if let Some(send) = send
                && send.session_id.as_deref() == Some(session_id.as_str())
                && send.host == ip.to_string()
            {
                send.cancel.by_peer.store(true, Ordering::Relaxed);
                send.cancel.token.cancel();
            }
        }
        ServerEventV2::ListenerFailed { error } => {
            ui.log(Category::Receive, &format!("Server stopped: {error}"));
        }
        // No receive session is ever accepted; dropping the responders
        // rejects the requests.
        ServerEventV2::FileUpload { .. }
        | ServerEventV2::SessionEnd { .. }
        | ServerEventV2::PrepareUploadAborted { .. } => {}
    }
}

fn handle_discovery(
    ui: &mut Ui,
    storage: &mut Repository,
    discovery: &DiscoveryHandle,
    event: DiscoveryEvent,
) {
    let device = match &event {
        DiscoveryEvent::Discovered { device } | DiscoveryEvent::Updated { device } => device,
        DiscoveryEvent::MulticastFailed => {
            ui.log(
                Category::Discovery,
                "Multicast stopped, no longer hearing announcements",
            );
            return;
        }
    };
    discovery::refresh_paired_channels(ui, &mut storage.paired, discovery, &device.fingerprint);

    let DiscoveryEvent::Discovered { device } = event else {
        // Re-confirmations update the store silently.
        return;
    };
    let host = device
        .http()
        .map(|http| http.host.as_str())
        .unwrap_or("unknown address");
    ui.log(
        Category::Discovery,
        &format!("{} ({host})", sanitize::single_line(&device.alias)),
    );
}

/// The status line: the progress bar of the running transfer.
fn render_progress(ui: &mut Ui, send: &mut Option<SendState>) {
    let Some(send) = send else {
        ui.set_status(None);
        return;
    };
    let done = send.sent.load(Ordering::Relaxed);
    let speed = send.speed.update(done);
    let width = util::terminal_width().saturating_sub(1);
    ui.set_status(Some(status::transfer_status(
        Category::Send,
        &send.alias,
        done,
        send.total_bytes,
        speed,
        width,
    )));
}

fn is_ip_allowed(ip_str: &str, allow_spec: Option<&str>, paired_contains: bool) -> bool {
    if paired_contains {
        return true;
    }
    let Some(spec) = allow_spec.map(str::trim).filter(|s| !s.is_empty()) else {
        return false;
    };
    if spec == "*" {
        return true;
    }
    let bare = ip_str.split('%').next().unwrap_or(ip_str).trim();
    let Ok(ip) = bare.parse::<IpAddr>() else {
        return false;
    };
    let canonical = match ip {
        IpAddr::V6(v6) => v6.to_ipv4_mapped().map(IpAddr::V4).unwrap_or(IpAddr::V6(v6)),
        IpAddr::V4(v4) => IpAddr::V4(v4),
    };
    spec.split(',').map(str::trim).any(|item| {
        if item == "*" {
            return true;
        }
        if let Ok(allowed_ip) = item.parse::<IpAddr>() {
            let allowed_canonical = match allowed_ip {
                IpAddr::V6(v6) => v6
                    .to_ipv4_mapped()
                    .map(IpAddr::V4)
                    .unwrap_or(IpAddr::V6(v6)),
                IpAddr::V4(v4) => IpAddr::V4(v4),
            };
            if canonical == allowed_canonical {
                return true;
            }
            if canonical.is_loopback() && allowed_canonical.is_loopback() {
                return true;
            }
        }
        false
    })
}

/// Runs the CLI in headless receive mode (no TTY/raw mode required).
pub(super) async fn run_receive(
    storage: Repository,
    auto_accept_from: Option<String>,
) -> anyhow::Result<()> {
    let identity = storage.identity.clone();
    let (events_tx, mut events_rx) = mpsc::channel::<AppEvent>(64);
    let Network {
        server,
        server_stop_tx,
        server_tx: _,
        mut server_rx,
        discovery,
        mut discovery_rx,
        discovery_stop_tx,
    } = start_network(&identity).await?;

    println!("LOCALSEND_RECEIVER_READY port={} dest={}", identity.port, storage.destination.display());

    let mut active_sessions: HashSet<String> = HashSet::new();
    let mut file_paths: HashMap<(String, String), PathBuf> = HashMap::new();

    loop {
        tokio::select! {
            _ = tokio::signal::ctrl_c() => {
                break;
            }
            Some(event) = server_rx.recv() => {
                match event {
                    ServerEventV2::Register { ip, info } => {
                        discovery::device_confirmed(
                            &discovery,
                            &storage.identity.fingerprint,
                            ip.to_string(),
                            info,
                        );
                    }
                    ServerEventV2::PrepareUpload {
                        session_id,
                        ip,
                        info,
                        cert_fingerprint,
                        files,
                        decision_tx,
                    } => {
                        let fp = cert_fingerprint.unwrap_or_else(|| info.fingerprint.clone());
                        let paired = storage.paired.contains(&fp);
                        let ip_str = ip.to_string();
                        if is_ip_allowed(&ip_str, auto_accept_from.as_deref(), paired) {
                            let ids: HashSet<String> = files.keys().cloned().collect();
                            if decision_tx.send(PrepareUploadDecisionV2::Accept(ids)).is_ok() {
                                println!("RECEIVE_SESSION_STARTED:{}:{}:{}", session_id, ip, files.len());
                                active_sessions.insert(session_id);
                            }
                        } else {
                            let _ = decision_tx.send(PrepareUploadDecisionV2::Decline);
                            println!("RECEIVE_DECLINED:{}", ip);
                        }
                    }
                    ServerEventV2::FileUpload {
                        session_id,
                        file_id,
                        file,
                        target_tx,
                    } => {
                        if !active_sessions.contains(&session_id) {
                            continue;
                        }
                        let path = util::unique_path(&storage.destination, &file.file_name);
                        file_paths.insert((session_id.clone(), file_id.clone()), path.clone());
                        let (result_tx, result_rx) = oneshot::channel::<Result<(), String>>();
                        let events_tx = events_tx.clone();
                        tokio::spawn(async move {
                            let result = match result_rx.await {
                                Ok(r) => r,
                                Err(_) => Err("Upload aborted".to_string()),
                            };
                            let _ = events_tx
                                .send(AppEvent::ReceiveFileResult {
                                    session_id,
                                    file_id,
                                    result,
                                })
                                .await;
                        });
                        let _ = target_tx.send(FileUploadTarget::Path {
                            path,
                            result_tx,
                            progress_tx: None,
                        });
                    }
                    ServerEventV2::SessionEnd { session_id, reason } => {
                        active_sessions.remove(&session_id);
                        match reason {
                            SessionEndReasonV2::Finished => {
                                println!("RECEIVE_SESSION_FINISHED:{}", session_id);
                            }
                            SessionEndReasonV2::Cancelled => {
                                println!("RECEIVE_SESSION_CANCELLED:{}", session_id);
                            }
                        }
                    }
                    ServerEventV2::PrepareUploadAborted { session_id } => {
                        active_sessions.remove(&session_id);
                    }
                    ServerEventV2::CancelReceived { .. } | ServerEventV2::ListenerFailed { .. } => {}
                }
            }
            Some(event) = events_rx.recv() => {
                if let AppEvent::ReceiveFileResult { session_id, file_id, result } = event {
                    if let Some(path) = file_paths.remove(&(session_id, file_id)) {
                        match result {
                            Ok(()) => println!("RECEIVED_FILE:{}", path.display()),
                            Err(err) => println!("RECEIVE_FILE_FAILED:{}:{}", path.display(), err),
                        }
                    }
                }
            }
            Some(_disc) = discovery_rx.recv() => {}
        }
    }

    stop_network(&server, Some(server_stop_tx), &discovery, discovery_stop_tx).await;
    Ok(())
}

