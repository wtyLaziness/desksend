//! The headless `send --to` mode: discover the destination, send the files
//! or the text message, exit with the transfer result. There is no keyboard and no overlay — only
//! the network events a send needs are handled, and incoming transfer
//! requests are declined.

use super::sending::{self, Payload, SendState};
use super::receive::SenderTarget;
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
use tokio_util::sync::CancellationToken;

pub(super) async fn run(
    mut storage: Repository,
    target: TargetSelector,
    payload: Payload,
    pin: Option<String>,
    cancel_stdin: bool,
) -> anyhow::Result<()> {
    let cancel = CancellationToken::new();
    if cancel_stdin {
        let cancel = cancel.clone();
        // 使用独立线程读管道，正常发送完成时无需等待父进程关闭标准输入。
        std::thread::spawn(move || {
            if std::io::stdin().lock().lines().map_while(Result::ok).any(|line| line == "CANCEL") {
                cancel.cancel();
            }
        });
    }

    let mut ui = Ui::new();
    let (events_tx, mut events_rx) = mpsc::channel::<AppEvent>(64);
    let Network {
        server,
        server_stop_tx,
        server_tx: _,
        mut server_rx,
        discovery,
        mut discovery_rx,
        discovery_stop_tx,
    } = start_network(&storage.identity).await?;
    Arc::make_mut(&mut storage.identity).port = server.port();
    let identity = storage.identity.clone();

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
    let mut cancel_requested = false;
    let result = loop {
        let event = tokio::select! {
            _ = cancel.cancelled(), if !cancel_requested => {
                let Some(send) = &send else {
                    println!("SEND_CANCELLED");
                    break Err(anyhow::anyhow!("Transfer cancelled"));
                };
                cancel_requested = true;
                send.cancel.token.cancel();
                continue;
            }
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
                let cancelled = cancel_requested || send.as_ref().is_some_and(|state| state.cancel.token.is_cancelled());
                if !success && cancelled {
                    println!("SEND_CANCELLED");
                }
                break match success {
                    true => Ok(()),
                    false if cancelled => Err(anyhow::anyhow!("Transfer cancelled")),
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
    cancelled: bool,
    sender: Option<SenderTarget>,
}

impl ReceiveProgress {
    fn report(&self, session_id: &str) {
        if self.cancelled {
            return;
        }
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
    Cancel(String),
}

fn parse_auth_command(line: &str) -> Option<AuthCommand> {
    let trimmed = line.trim();
    if let Some(token) = trimmed.strip_prefix("GRANT ") {
        let token = token.trim();
        if !token.is_empty() && token.len() <= 256 && !token.chars().any(|ch| ch.is_control() || ch.is_whitespace()) {
            return Some(AuthCommand::Grant(token.to_string()));
        }
    } else if let Some(token) = trimmed.strip_prefix("REVOKE ") {
        let token = token.trim();
        if !token.is_empty() && token.len() <= 256 && !token.chars().any(|ch| ch.is_control() || ch.is_whitespace()) {
            return Some(AuthCommand::Revoke(token.to_string()));
        }
    } else if let Some(id) = trimmed.strip_prefix("CANCEL ")
        && uuid::Uuid::parse_str(id).is_ok()
    {
        return Some(AuthCommand::Cancel(id.to_string()));
    }
    None
}

/// Runs the CLI in headless receive mode (no TTY/raw mode required).
pub(super) async fn run_receive(
    storage: Repository,
    auto_accept_from: Option<String>,
    auth_stdin: bool,
    received_paths_hex: bool,
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

    println!("LOCALSEND_RECEIVER_READY port={} dest={}", server.port(), storage.destination.display());

    let mut active_sessions: HashSet<String> = HashSet::new();
    let mut file_paths: HashMap<(String, String), PathBuf> = HashMap::new();
    let mut progress: HashMap<String, ReceiveProgress> = HashMap::new();
    let mut tick = tokio::time::interval(Duration::from_millis(250));
    tick.set_missed_tick_behavior(tokio::time::MissedTickBehavior::Skip);

    let result = loop {
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
                    Some(AuthCommand::Cancel(session_id)) => {
                        if active_sessions.contains(&session_id) && server.cancel_v2_session(&session_id).await {
                            let sender = progress.get(&session_id).and_then(|state| state.sender.clone());
                            active_sessions.remove(&session_id);
                            end_receive_session(&mut progress, &file_paths, &session_id, SessionEndReasonV2::Cancelled, true);
                            if let Some(sender) = sender {
                                let identity = identity.clone();
                                tokio::spawn(async move {
                                    sender.notify_cancel(&identity, &session_id).await;
                                });
                            }
                        }
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
                break Ok(());
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
                                    sender: auth_stdin.then(|| SenderTarget {
                                        host: ip_str,
                                        port: info.port,
                                        protocol: info.protocol,
                                        fingerprint: fp,
                                    }),
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
                        let path = util::unique_nested_path(&storage.destination, &file.file_name, received_paths_hex);
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
                        let target = if auth_stdin {
                            FileUploadTarget::StagedPath { path, result_tx, progress_tx: Some(progress_tx) }
                        } else {
                            FileUploadTarget::Path { path, result_tx, progress_tx: Some(progress_tx) }
                        };
                        let _ = target_tx.send(target);
                    }
                    ServerEventV2::SessionEnd { session_id, reason } => {
                        if active_sessions.remove(&session_id) {
                            end_receive_session(&mut progress, &file_paths, &session_id, reason, auth_stdin);
                        }
                    }
                    ServerEventV2::PrepareUploadAborted { session_id } => {
                        if active_sessions.remove(&session_id) {
                            end_receive_session(&mut progress, &file_paths, &session_id, SessionEndReasonV2::Cancelled, auth_stdin);
                        }
                    }
                    ServerEventV2::CancelReceived { .. } => {}
                    ServerEventV2::ListenerFailed { error } => break Err(anyhow::anyhow!("Receiver listener stopped: {error}")),
                }
            }
            Some(event) = events_rx.recv() => {
                if let AppEvent::ReceiveFileResult { session_id, file_id, result } = event {
                    let mut cancelled = false;
                    if let Some(state) = progress.get_mut(&session_id) {
                        cancelled = state.cancelled;
                        state.report(&session_id);
                        state.pending = state.pending.saturating_sub(1);
                        if state.ended && state.pending == 0 {
                            progress.remove(&session_id);
                        }
                    }
                    if let Some(path) = file_paths.remove(&(session_id, file_id))
                        && !cancelled
                    {
                        match result {
                            Ok(()) if received_paths_hex => {
                                if let Some(path) = path.to_str() {
                                    println!("RECEIVED_FILE_HEX:{}", util::encode_path_hex(path));
                                } else {
                                    println!("RECEIVE_FILE_FAILED:invalid UTF-8 path");
                                }
                            }
                            Ok(()) => println!("RECEIVED_FILE:{}", path.display()),
                            Err(err) => println!("RECEIVE_FILE_FAILED:{}:{}", sanitize::single_line(&path.to_string_lossy()), sanitize::single_line(&err.to_string())),
                        }
                    }
                }
            }
            Some(_disc) = discovery_rx.recv() => {}
        }
    };

    stop_network(&server, Some(server_stop_tx), &discovery, discovery_stop_tx).await;
    result
}

fn end_receive_session(
    progress: &mut HashMap<String, ReceiveProgress>,
    file_paths: &HashMap<(String, String), PathBuf>,
    session_id: &str,
    reason: SessionEndReasonV2,
    auth_stdin: bool,
) {
    if let Some(state) = progress.get_mut(session_id) {
        state.ended = true;
        if reason == SessionEndReasonV2::Cancelled {
            // 已取消的旧会话可能还有写入任务在收尾，其结果不应计入下一批。
            state.cancelled = true;
            state.pending = if auth_stdin {
                file_paths.keys().filter(|(id, _)| id == session_id).count()
            } else {
                0
            };
        }
        state.report(session_id);
        if state.pending == 0 {
            progress.remove(session_id);
        }
    }
    match reason {
        SessionEndReasonV2::Finished => println!("RECEIVE_SESSION_FINISHED:{session_id}"),
        SessionEndReasonV2::Cancelled => println!("RECEIVE_SESSION_CANCELLED:{session_id}"),
    }
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

