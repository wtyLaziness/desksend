//! The headless `send --to` mode: discover the destination, send the files
//! or the text message, exit with the transfer result. There is no keyboard and no overlay — only
//! the network events a send needs are handled, and incoming transfer
//! requests are declined.

use super::sending::{self, Payload, SendState};
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
use std::io::BufRead;
use std::net::IpAddr;
use std::path::PathBuf;
use std::sync::Arc;
use std::sync::atomic::{AtomicU64, Ordering};
use std::time::Duration;
use tokio::sync::{mpsc, oneshot};

pub(super) async fn run(
    mut storage: Repository,
    target: TargetSelector,
    payload: Payload,
    pin: Option<String>,
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
    let direct_channel = target.direct_channel();
    let mut known_channels = storage.paired.known_http_channels();
    if let Some(channel) = &direct_channel
        && !known_channels.contains(channel)
    {
        known_channels.insert(0, channel.clone());
    }
    spawn_staged_discovery(
        discovery.clone(),
        events_tx.clone(),
        known_channels,
        direct_channel,
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
                match start_send(
                    &mut ui,
                    &storage,
                    &discovery,
                    &target,
                    &payload,
                    pin.clone(),
                    &events_tx,
                ) {
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
                    println!("SEND_PROGRESS:0:{}", accepted_bytes);
                }
            }
            AppEvent::SendEnded { success } => {
                if success && let Some(send) = &send {
                    println!("SEND_PROGRESS:{}:{}", send.total_bytes, send.total_bytes);
                }
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
    pin: Option<String>,
    events_tx: &mpsc::Sender<AppEvent>,
) -> Result<SendState, String> {
    let fingerprint = target.resolve(&discovery.devices())?;
    let mut device = discovery
        .device_by_fingerprint(&fingerprint)
        .ok_or_else(|| format!("Destination {target} was not discovered"))?;
    if let Some(direct_http) = target.direct_channel() {
        let direct_ch = localsend::discovery::DeviceChannel::Http(direct_http);
        device.device.channel = direct_ch.clone();
        device.channels.retain(|ch, _| *ch == direct_ch);
    }
    match payload {
        Payload::Files(paths) => sending::spawn_send(
            ui,
            storage.identity.clone(),
            device,
            paths.clone(),
            pin,
            events_tx.clone(),
        ),
        Payload::Text(text) => sending::spawn_send_text(
            ui,
            storage.identity.clone(),
            device,
            text.clone(),
            pin,
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
    println!("SEND_PROGRESS:{}:{}", done, send.total_bytes);
}

#[derive(Default)]
struct ReceiveProgress {
    total: u64,
    files: HashMap<String, Arc<AtomicU64>>,
    pending: usize,
    ended: bool,
}

impl ReceiveProgress {
    fn report(&self, session_id: &str) {
        let done: u64 = self.files.values().map(|p| p.load(Ordering::Relaxed)).sum();
        println!("RECEIVE_PROGRESS:{}:{}:{}", session_id, done.min(self.total), self.total);
    }
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
            if allowed_canonical.is_loopback()
                && let IpAddr::V4(v4) = canonical
                && localsend::util::interface::local_interface_addresses(
                    &localsend::util::interface::InterfaceFilter::default(),
                )
                .is_ok_and(|addrs| addrs.contains(&v4))
            {
                return true;
            }
        }
        false
    })
}

#[derive(Debug, PartialEq, Eq)]
enum AuthCommand {
    Grant(String),
    Revoke(String),
}

fn parse_auth_command(line: &str) -> Option<AuthCommand> {
    let trimmed = line.trim();
    if let Some(token) = trimmed.strip_prefix("GRANT ") {
        let token = token.trim();
        if !token.is_empty() {
            return Some(AuthCommand::Grant(token.to_string()));
        }
    } else if let Some(token) = trimmed.strip_prefix("REVOKE ") {
        let token = token.trim();
        if !token.is_empty() {
            return Some(AuthCommand::Revoke(token.to_string()));
        }
    }
    None
}

/// Runs the CLI in headless receive mode (no TTY/raw mode required).
pub(super) async fn run_receive(
    storage: Repository,
    auto_accept_from: Option<String>,
    auth_stdin: bool,
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

    let mut auth_rx = if auth_stdin {
        server.enable_v2_token_gate().await;
        let (tx, rx) = mpsc::channel::<AuthCommand>(64);
        std::thread::spawn(move || {
            let stdin = std::io::stdin();
            for line in std::io::BufReader::new(stdin.lock()).lines() {
                let Ok(line) = line else {
                    break;
                };
                if let Some(cmd) = parse_auth_command(&line)
                    && tx.blocking_send(cmd).is_err()
                {
                    break;
                }
            }
        });
        Some(rx)
    } else {
        None
    };

    println!("LOCALSEND_RECEIVER_READY port={} dest={}", identity.port, storage.destination.display());

    let mut active_sessions: HashSet<String> = HashSet::new();
    let mut file_paths: HashMap<(String, String), PathBuf> = HashMap::new();
    let mut progress: HashMap<String, ReceiveProgress> = HashMap::new();
    let mut tick = tokio::time::interval(Duration::from_millis(250));
    tick.set_missed_tick_behavior(tokio::time::MissedTickBehavior::Skip);

    loop {
        tokio::select! {
            auth_cmd = async {
                match auth_rx.as_mut() {
                    Some(rx) => rx.recv().await,
                    None => std::future::pending().await,
                }
            } => {
                match auth_cmd {
                    Some(AuthCommand::Grant(token)) => {
                        server.grant_v2_token(token).await;
                    }
                    Some(AuthCommand::Revoke(token)) => {
                        server.revoke_v2_token(&token).await;
                    }
                    None => {
                        auth_rx = None;
                    }
                }
            }
            _ = tick.tick() => {
                for (sid, state) in &progress {
                    state.report(sid);
                }
            }
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
                        pin_tag,
                        decision_tx,
                    } => {
                        let has_client_cert = cert_fingerprint.is_some();
                        let fp = cert_fingerprint.unwrap_or_else(|| info.fingerprint.clone());
                        let paired = !auth_stdin && storage.paired.contains(&fp);
                        let ip_str = ip.to_string();
                        let authorized = (!auth_stdin || has_client_cert)
                            && is_ip_allowed(&ip_str, auto_accept_from.as_deref(), paired);
                        if authorized {
                            let ids: HashSet<String> = files.keys().cloned().collect();
                            if decision_tx.send(PrepareUploadDecisionV2::Accept(ids)).is_ok() {
                                match pin_tag.as_deref() {
                                    Some(tag) => println!(
                                        "RECEIVE_SESSION_STARTED:{}:{}:{}:tok={}",
                                        session_id,
                                        ip,
                                        files.len(),
                                        tag
                                    ),
                                    None => println!(
                                        "RECEIVE_SESSION_STARTED:{}:{}:{}",
                                        session_id,
                                        ip,
                                        files.len()
                                    ),
                                }
                                let state = ReceiveProgress {
                                    total: files.values().map(|f| f.size).sum(),
                                    pending: files.len(),
                                    ..Default::default()
                                };
                                state.report(&session_id);
                                progress.insert(session_id.clone(), state);
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
                        let path = util::unique_nested_path(&storage.destination, &file.file_name);
                        file_paths.insert((session_id.clone(), file_id.clone()), path.clone());
                        let (result_tx, result_rx) = oneshot::channel::<Result<(), String>>();
                        let bytes = Arc::new(AtomicU64::new(0));
                        if let Some(state) = progress.get_mut(&session_id) {
                            state.files.insert(file_id.clone(), bytes.clone());
                        }
                        let (progress_tx, mut progress_rx) = mpsc::channel::<u64>(16);
                        let final_bytes = bytes.clone();
                        let file_size = file.size;
                        let progress_task = tokio::spawn(async move {
                            while let Some(written) = progress_rx.recv().await {
                                bytes.store(written, Ordering::Relaxed);
                            }
                        });
                        let events_tx = events_tx.clone();
                        tokio::spawn(async move {
                            let result = match result_rx.await {
                                Ok(r) => r,
                                Err(_) => Err("Upload aborted".to_string()),
                            };
                            progress_task.abort();
                            let _ = progress_task.await;
                            if result.is_ok() {
                                final_bytes.store(file_size, Ordering::Relaxed);
                            }
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
                            progress_tx: Some(progress_tx),
                        });
                    }
                    ServerEventV2::SessionEnd { session_id, reason } => {
                        active_sessions.remove(&session_id);
                        if let Some(state) = progress.get_mut(&session_id) {
                            state.ended = true;
                            state.report(&session_id);
                            if state.pending == 0 {
                                progress.remove(&session_id);
                            }
                        }
                        match reason {
                            SessionEndReasonV2::Finished => {
                                println!("RECEIVE_SESSION_FINISHED:{}", session_id);
                            }
                            SessionEndReasonV2::Cancelled => {
                                progress.remove(&session_id);
                                println!("RECEIVE_SESSION_CANCELLED:{}", session_id);
                            }
                        }
                    }
                    ServerEventV2::PrepareUploadAborted { session_id } => {
                        active_sessions.remove(&session_id);
                        progress.remove(&session_id);
                    }
                    ServerEventV2::CancelReceived { .. } | ServerEventV2::ListenerFailed { .. } => {}
                }
            }
            Some(event) = events_rx.recv() => {
                if let AppEvent::ReceiveFileResult { session_id, file_id, result } = event {
                    if let Some(state) = progress.get_mut(&session_id) {
                        state.report(&session_id);
                        state.pending = state.pending.saturating_sub(1);
                        if state.ended && state.pending == 0 {
                            progress.remove(&session_id);
                        }
                    }
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

#[cfg(test)]
mod tests {
    use super::{AuthCommand, parse_auth_command};

    #[test]
    fn parse_auth_command_handles_valid_and_invalid_lines() {
        assert_eq!(
            parse_auth_command("GRANT deadbeef0123\r\n"),
            Some(AuthCommand::Grant("deadbeef0123".to_string()))
        );
        assert_eq!(
            parse_auth_command("  REVOKE abc  "),
            Some(AuthCommand::Revoke("abc".to_string()))
        );
        assert_eq!(parse_auth_command("GRANT   "), None);
        assert_eq!(parse_auth_command("REVOKE"), None);
        assert_eq!(parse_auth_command("UNKNOWN abc"), None);
    }
}

