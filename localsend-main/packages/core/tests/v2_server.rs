#![cfg(feature = "http")]

use bytes::Bytes;
use futures_util::StreamExt;
use localsend::crypto::hash::sha256_hex;
use localsend::http::client::{ClientError, LsHttpClient, LsHttpClientV2};
use localsend::http::dto_v2::{PrepareUploadRequestDtoV2, RegisterDtoV2};
use localsend::http::server::common::save::FileUploadTarget;
use localsend::http::server::v2::{PrepareUploadDecisionV2, ServerEventV2, SessionEndReasonV2};
use localsend::http::server::web::WebConfig;
use localsend::http::server::{start_with_port, start_with_port_and_limits, ServerConfigV2, ServerHandle, V2Limits};
use localsend::http::state::ClientInfo;
use localsend::model::discovery::ProtocolType;
use localsend::model::transfer::{FileContent, FileDto, FileMetadata};
use std::collections::HashMap;
use std::path::PathBuf;
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::Arc;
use std::time::Duration;
use tokio::sync::{mpsc, oneshot, Mutex};
use tokio_stream::wrappers::ReceiverStream;
use tokio_util::sync::CancellationToken;

struct TestServer {
    port: u16,
    handle: ServerHandle,
    /// Uploaded file contents, mapped by file ID.
    received: Arc<Mutex<HashMap<String, Vec<u8>>>>,
    /// Ended sessions with their reasons.
    session_ends: Arc<Mutex<Vec<(String, SessionEndReasonV2)>>>,
    _stop_tx: oneshot::Sender<()>,
}

/// Starts a test server.
///
/// Uploads are received as a stream, or written by the server into `save_dir`
/// when given. Either way, the content ends up in [`TestServer::received`].
async fn start_test_server(
    pin: Option<String>,
    accept: bool,
    save_dir: Option<PathBuf>,
) -> TestServer {
    start_test_server_with_verification(pin, accept, save_dir, true, false).await
}

/// Like [start_test_server], but allows disabling the checksum verification.
async fn start_test_server_with_verification(
    pin: Option<String>,
    accept: bool,
    save_dir: Option<PathBuf>,
    verify_checksums: bool,
    stage_uploads: bool,
) -> TestServer {
    let _ = tracing_subscriber::fmt().with_test_writer().try_init();
    let received: Arc<Mutex<HashMap<String, Vec<u8>>>> = Arc::new(Mutex::new(HashMap::new()));
    let session_ends: Arc<Mutex<Vec<(String, SessionEndReasonV2)>>> =
        Arc::new(Mutex::new(Vec::new()));

    let (event_tx, mut event_rx) = mpsc::channel::<ServerEventV2>(16);

    tokio::spawn({
        let received = received.clone();
        let session_ends = session_ends.clone();
        async move {
            while let Some(event) = event_rx.recv().await {
                match event {
                    ServerEventV2::Register { .. } => {}
                    ServerEventV2::PrepareUpload {
                        files, decision_tx, ..
                    } => {
                        let decision = match accept {
                            true => {
                                PrepareUploadDecisionV2::Accept(files.keys().cloned().collect())
                            }
                            false => PrepareUploadDecisionV2::Decline,
                        };
                        let _ = decision_tx.send(decision);
                    }
                    ServerEventV2::FileUpload {
                        file_id, target_tx, ..
                    } => {
                        let received = received.clone();
                        match &save_dir {
                            None => {
                                let (binary_tx, mut binary_rx) = mpsc::channel(16);
                                let (result_tx, result_rx) = oneshot::channel();
                                let _ = target_tx.send(FileUploadTarget::Stream {
                                    binary_tx,
                                    result_rx,
                                });
                                tokio::spawn(async move {
                                    let mut bytes = Vec::new();
                                    while let Some(chunk) = binary_rx.recv().await {
                                        bytes.extend_from_slice(&chunk);
                                    }
                                    received.lock().await.insert(file_id, bytes);
                                    let _ = result_tx.send(Ok(()));
                                });
                            }
                            Some(dir) => {
                                let path = dir.join(&file_id);
                                let (result_tx, result_rx) = oneshot::channel();
                                let target = if stage_uploads {
                                    FileUploadTarget::StagedPath {
                                        path: path.clone(),
                                        result_tx,
                                        progress_tx: None,
                                    }
                                } else {
                                    FileUploadTarget::Path {
                                        path: path.clone(),
                                        result_tx,
                                        progress_tx: None,
                                    }
                                };
                                let _ = target_tx.send(target);
                                tokio::spawn(async move {
                                    if let Ok(Ok(())) = result_rx.await {
                                        let bytes = tokio::fs::read(&path).await.unwrap();
                                        received.lock().await.insert(file_id, bytes);
                                    }
                                });
                            }
                        }
                    }
                    ServerEventV2::SessionEnd { session_id, reason } => {
                        session_ends.lock().await.push((session_id, reason));
                    }
                    ServerEventV2::PrepareUploadAborted { .. } => {}
                    ServerEventV2::CancelReceived { .. } => {}
                    ServerEventV2::ListenerFailed { .. } => {}
                }
            }
        }
    });

    let (stop_tx, stop_rx) = oneshot::channel::<()>();

    // Port 0 lets the OS pick a free port, avoiding collisions between tests.
    let handle = start_with_port(
        0,
        None, // plain HTTP
        ClientInfo {
            alias: "Test Server".to_string(),
            version: "2.2".to_string(),
            device_model: Some("Rust".to_string()),
            device_type: None,
            token: "server-fingerprint".to_string(),
        },
        None,
        Some(ServerConfigV2 {
            pin,
            verify_checksums,
            event_tx,
        }),
        WebConfig::default(),
        stop_rx,
    )
    .await
    .expect("Failed to start server");

    TestServer {
        port: handle.port(),
        handle,
        received,
        session_ends,
        _stop_tx: stop_tx,
    }
}

fn sender_info() -> RegisterDtoV2 {
    RegisterDtoV2 {
        alias: "Test Sender".to_string(),
        version: "2.2".to_string(),
        device_model: Some("Rust".to_string()),
        device_type: None,
        fingerprint: "sender-fingerprint".to_string(),
        port: 53317,
        protocol: ProtocolType::Http,
        download: false,
    }
}

fn file_dto(id: &str, name: &str, size: u64) -> FileDto {
    FileDto {
        id: id.to_string(),
        file_name: name.to_string(),
        size,
        file_type: "application/octet-stream".to_string(),
        sha256: None,
        preview: None,
        metadata: None,
    }
}

fn prepare_upload_request(files: &[FileDto]) -> PrepareUploadRequestDtoV2 {
    PrepareUploadRequestDtoV2 {
        info: sender_info(),
        files: files
            .iter()
            .map(|file| (file.id.clone(), file.clone()))
            .collect(),
    }
}

async fn upload_bytes(
    client: &LsHttpClientV2,
    port: u16,
    session_id: &str,
    file_id: &str,
    token: &str,
    bytes: &[u8],
) -> Result<(), ClientError> {
    let (tx, rx) = mpsc::channel::<Bytes>(4);
    let chunks: Vec<Vec<u8>> = bytes.chunks(1024).map(|chunk| chunk.to_vec()).collect();
    let sent = Arc::new(AtomicU64::new(0));
    tokio::spawn(async move {
        for chunk in chunks {
            if tx.send(Bytes::from(chunk)).await.is_err() {
                break;
            }
        }
    });

    // The caller now owns building the request body; track cumulative bytes
    // sent so the progress assertion below still holds.
    let progress = sent.clone();
    let body =
        localsend::reqwest::Body::wrap_stream(ReceiverStream::new(rx).map(move |chunk: Bytes| {
            progress.fetch_add(chunk.len() as u64, Ordering::Relaxed);
            Ok::<Bytes, std::io::Error>(chunk)
        }));
    let result = client
        .upload(
            ProtocolType::Http,
            "127.0.0.1",
            port,
            None,
            session_id,
            file_id,
            token,
            body,
            CancellationToken::new(),
        )
        .await;

    if result.is_ok() {
        assert_eq!(sent.load(Ordering::Relaxed), bytes.len() as u64);
    }

    result
}

fn assert_status(result: Result<impl Sized, ClientError>, expected_status: u16) {
    match result {
        Err(ClientError::StatusCode(err)) => assert_eq!(err.status, expected_status),
        Err(err) => panic!("Expected status code {expected_status}, got error: {err:?}"),
        Ok(_) => panic!("Expected status code {expected_status}, got success"),
    }
}

#[tokio::test]
async fn oversized_chunked_nonce_is_rejected_before_end_of_body() {
    use tokio::io::{AsyncReadExt, AsyncWriteExt};

    let server = start_test_server(None, false, None).await;
    let mut stream = tokio::net::TcpStream::connect(("127.0.0.1", server.port)).await.unwrap();
    stream.write_all(b"POST /api/localsend/v3/nonce HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: chunked\r\n\r\n").await.unwrap();
    for _ in 0..3 {
        stream.write_all(b"200\r\n").await.unwrap();
        stream.write_all(&[b'x'; 512]).await.unwrap();
        stream.write_all(b"\r\n").await.unwrap();
    }
    let mut status = [0u8; 12];
    tokio::time::timeout(Duration::from_secs(2), stream.read_exact(&mut status)).await.unwrap().unwrap();
    assert_eq!(&status, b"HTTP/1.1 413");
}

#[tokio::test]
async fn token_gate_is_active_before_first_request_and_idle_session_expires() {
    let (event_tx, mut events) = mpsc::channel(8);
    let (_stop_tx, stop_rx) = oneshot::channel();
    let handle = start_with_port_and_limits(
        0, None,
        ClientInfo {
            alias: "receiver".into(), version: "2.2".into(), device_model: None,
            device_type: None, token: "fingerprint".into(),
        },
        None, Some(ServerConfigV2 { pin: None, verify_checksums: true, event_tx }),
        WebConfig::default(), stop_rx,
        V2Limits { require_token: true, session_idle_timeout: Duration::from_millis(100), ..V2Limits::default() },
    ).await.unwrap();
    let client = LsHttpClientV2::try_new_without_cert().unwrap();
    let prepare = |pin| client.prepare_upload(
        ProtocolType::Http, "127.0.0.1", handle.port(), None,
        prepare_upload_request(&[file_dto("file-a", "a.txt", 5)]), pin, CancellationToken::new(),
    );
    assert_status(prepare(None).await, 401);
    assert!(events.try_recv().is_err());
    for secret in ["first-token", "second-token"] {
        handle.grant_v2_token(secret.into()).await;
        let decide = async {
            let ServerEventV2::PrepareUpload { files, decision_tx, token_authenticated, .. } = events.recv().await.unwrap() else {
                panic!("expected prepare event");
            };
            assert!(token_authenticated);
            decision_tx.send(PrepareUploadDecisionV2::Accept(files.keys().cloned().collect())).unwrap();
        };
        let (reply, ()) = tokio::join!(prepare(Some(secret)), decide);
        assert!(reply.is_ok());
        assert!(matches!(
            tokio::time::timeout(Duration::from_secs(2), events.recv()).await.unwrap(),
            Some(ServerEventV2::SessionEnd { reason: SessionEndReasonV2::Cancelled, .. })
        ));
    }
}

#[tokio::test]
async fn test_register_and_info() {
    let server = start_test_server(None, true, None).await;
    let client = LsHttpClientV2::try_new_without_cert().unwrap();

    let response = client
        .register(ProtocolType::Http, "127.0.0.1", server.port, sender_info())
        .await
        .unwrap();
    assert_eq!(response.body.alias, "Test Server");
    assert_eq!(response.body.fingerprint, "server-fingerprint");
    assert!(!response.body.download);

    let info = client
        .info(ProtocolType::Http, "127.0.0.1", server.port)
        .await
        .unwrap();
    assert_eq!(info.alias, "Test Server");
    assert_eq!(info.fingerprint, "server-fingerprint");
}

/// Old clients (v1.17 and earlier) probe unknown peers on the legacy v1 route.
#[tokio::test]
async fn test_info_on_legacy_v1_route() {
    let server = start_test_server(None, true, None).await;

    let body: serde_json::Value = reqwest::get(format!(
        "http://127.0.0.1:{}/api/localsend/v1/info",
        server.port
    ))
    .await
    .unwrap()
    .error_for_status()
    .unwrap()
    .json()
    .await
    .unwrap();

    assert_eq!(body["alias"], "Test Server");
    assert_eq!(body["fingerprint"], "server-fingerprint");
}

#[tokio::test]
async fn test_register_over_ipv6() {
    let server = start_test_server(None, true, None).await;
    let client = LsHttpClientV2::try_new_without_cert().unwrap();

    let response = client
        .register(ProtocolType::Http, "::1", server.port, sender_info())
        .await
        .unwrap();
    assert_eq!(response.body.alias, "Test Server");
}

#[tokio::test]
async fn test_full_upload_flow() {
    let server = start_test_server(None, true, None).await;
    let client = LsHttpClientV2::try_new_without_cert().unwrap();

    let file_a = file_dto("file-a", "a.bin", 100_000);
    let file_b = file_dto("file-b", "b.bin", 5);

    let result = client
        .prepare_upload(
            ProtocolType::Http,
            "127.0.0.1",
            server.port,
            None,
            prepare_upload_request(&[file_a.clone(), file_b.clone()]),
            None,
            CancellationToken::new(),
        )
        .await
        .unwrap();

    assert_eq!(result.status_code, 200);
    let response = result.response.unwrap();
    assert_eq!(response.files.len(), 2);

    let bytes_a: Vec<u8> = (0..100_000u32).map(|i| i as u8).collect();
    let bytes_b = b"hello".to_vec();

    upload_bytes(
        &client,
        server.port,
        &response.session_id,
        "file-a",
        &response.files["file-a"],
        &bytes_a,
    )
    .await
    .unwrap();

    upload_bytes(
        &client,
        server.port,
        &response.session_id,
        "file-b",
        &response.files["file-b"],
        &bytes_b,
    )
    .await
    .unwrap();

    let received = server.received.lock().await;
    assert_eq!(received["file-a"], bytes_a);
    assert_eq!(received["file-b"], bytes_b);
    drop(received);

    // The session should have ended after all files were uploaded.
    tokio::time::sleep(Duration::from_millis(100)).await;
    let session_ends = server.session_ends.lock().await;
    assert_eq!(
        *session_ends,
        vec![(response.session_id.clone(), SessionEndReasonV2::Finished)]
    );

    // The session is gone, so uploading again is rejected.
    let result = upload_bytes(
        &client,
        server.port,
        &response.session_id,
        "file-a",
        &response.files["file-a"],
        b"again",
    )
    .await;
    assert_status(result, 403);
}

/// The sender-provided metadata timestamps are applied to the written file.
#[tokio::test]
async fn test_upload_applies_file_timestamps() {
    let save_dir = std::env::temp_dir().join(format!("localsend-test-{}", uuid::Uuid::new_v4()));
    tokio::fs::create_dir_all(&save_dir).await.unwrap();

    let server = start_test_server(None, true, Some(save_dir.clone())).await;
    let client = LsHttpClientV2::try_new_without_cert().unwrap();

    let bytes = b"hello".to_vec();
    let mut file = file_dto("file-a", "a.bin", bytes.len() as u64);
    file.metadata = Some(FileMetadata {
        modified: Some("2020-08-15T10:20:30.500Z".to_string()),
        accessed: None,
    });

    let response = client
        .prepare_upload(
            ProtocolType::Http,
            "127.0.0.1",
            server.port,
            None,
            prepare_upload_request(&[file]),
            None,
            CancellationToken::new(),
        )
        .await
        .unwrap()
        .response
        .unwrap();

    upload_bytes(
        &client,
        server.port,
        &response.session_id,
        "file-a",
        &response.files["file-a"],
        &bytes,
    )
    .await
    .unwrap();

    let modified = tokio::fs::metadata(save_dir.join("file-a"))
        .await
        .unwrap()
        .modified()
        .unwrap();
    assert_eq!(
        modified,
        std::time::SystemTime::UNIX_EPOCH + Duration::from_millis(1_597_486_830_500),
    );

    tokio::fs::remove_dir_all(&save_dir).await.unwrap();
}

#[tokio::test]
async fn test_upload_with_matching_sha256() {
    let server = start_test_server(None, true, None).await;
    let client = LsHttpClientV2::try_new_without_cert().unwrap();

    let bytes = b"hello".to_vec();
    let mut file = file_dto("file-a", "a.bin", bytes.len() as u64);
    file.sha256 = Some(sha256_hex(&bytes));

    let response = client
        .prepare_upload(
            ProtocolType::Http,
            "127.0.0.1",
            server.port,
            None,
            prepare_upload_request(&[file]),
            None,
            CancellationToken::new(),
        )
        .await
        .unwrap()
        .response
        .unwrap();

    upload_bytes(
        &client,
        server.port,
        &response.session_id,
        "file-a",
        &response.files["file-a"],
        &bytes,
    )
    .await
    .unwrap();

    assert_eq!(server.received.lock().await["file-a"], bytes);
}

#[tokio::test]
async fn test_upload_with_mismatched_sha256() {
    let server = start_test_server(None, true, None).await;
    let client = LsHttpClientV2::try_new_without_cert().unwrap();

    let bytes = b"hello".to_vec();
    let mut file = file_dto("file-a", "a.bin", bytes.len() as u64);
    file.sha256 = Some(sha256_hex(b"something else"));

    let response = client
        .prepare_upload(
            ProtocolType::Http,
            "127.0.0.1",
            server.port,
            None,
            prepare_upload_request(&[file]),
            None,
            CancellationToken::new(),
        )
        .await
        .unwrap()
        .response
        .unwrap();

    let result = upload_bytes(
        &client,
        server.port,
        &response.session_id,
        "file-a",
        &response.files["file-a"],
        &bytes,
    )
    .await;
    assert_status(result, 422);
}

#[tokio::test]
async fn test_upload_mismatched_sha256_with_verification_disabled() {
    let server = start_test_server_with_verification(None, true, None, false, false).await;
    let client = LsHttpClientV2::try_new_without_cert().unwrap();

    let bytes = b"hello".to_vec();
    let mut file = file_dto("file-a", "a.bin", bytes.len() as u64);
    file.sha256 = Some(sha256_hex(b"something else"));

    let response = client
        .prepare_upload(
            ProtocolType::Http,
            "127.0.0.1",
            server.port,
            None,
            prepare_upload_request(&[file]),
            None,
            CancellationToken::new(),
        )
        .await
        .unwrap()
        .response
        .unwrap();

    // The mismatch goes unnoticed because the received bytes are not hashed.
    upload_bytes(
        &client,
        server.port,
        &response.session_id,
        "file-a",
        &response.files["file-a"],
        &bytes,
    )
    .await
    .unwrap();

    assert_eq!(server.received.lock().await["file-a"], bytes);
}

#[tokio::test]
async fn test_upload_retry_after_mismatched_sha256() {
    let server = start_test_server(None, true, None).await;
    let client = LsHttpClientV2::try_new_without_cert().unwrap();

    let bytes = b"hello".to_vec();
    let corrupted = b"hellO".to_vec();
    let mut file = file_dto("file-a", "a.bin", bytes.len() as u64);
    file.sha256 = Some(sha256_hex(&bytes));

    let response = client
        .prepare_upload(
            ProtocolType::Http,
            "127.0.0.1",
            server.port,
            None,
            prepare_upload_request(&[file]),
            None,
            CancellationToken::new(),
        )
        .await
        .unwrap()
        .response
        .unwrap();

    // The first upload is rejected because of the checksum mismatch ...
    let result = upload_bytes(
        &client,
        server.port,
        &response.session_id,
        "file-a",
        &response.files["file-a"],
        &corrupted,
    )
    .await;
    assert_status(result, 422);

    // ... but a retry with the same token is accepted.
    upload_bytes(
        &client,
        server.port,
        &response.session_id,
        "file-a",
        &response.files["file-a"],
        &bytes,
    )
    .await
    .unwrap();

    assert_eq!(server.received.lock().await["file-a"], bytes);

    tokio::time::sleep(Duration::from_millis(100)).await;
    let session_ends = server.session_ends.lock().await;
    assert_eq!(session_ends.len(), 1);
    assert_eq!(session_ends[0].0, response.session_id);
    assert!(matches!(session_ends[0].1, SessionEndReasonV2::Finished));
}

/// A retry writes to the same path as the failed attempt, so the receiver ends
/// up with exactly one file holding the correct content.
#[tokio::test]
async fn test_upload_retry_reuses_the_same_path() {
    let save_dir = std::env::temp_dir().join(format!("localsend-test-{}", uuid::Uuid::new_v4()));
    tokio::fs::create_dir_all(&save_dir).await.unwrap();

    let server = start_test_server(None, true, Some(save_dir.clone())).await;
    let client = LsHttpClientV2::try_new_without_cert().unwrap();

    let bytes: Vec<u8> = (0..50_000u32).map(|i| i as u8).collect();
    let mut corrupted = bytes.clone();
    *corrupted.last_mut().unwrap() ^= 0xff;
    let mut file = file_dto("file-a", "a.bin", bytes.len() as u64);
    file.sha256 = Some(sha256_hex(&bytes));

    let response = client
        .prepare_upload(
            ProtocolType::Http,
            "127.0.0.1",
            server.port,
            None,
            prepare_upload_request(&[file]),
            None,
            CancellationToken::new(),
        )
        .await
        .unwrap()
        .response
        .unwrap();

    let result = upload_bytes(
        &client,
        server.port,
        &response.session_id,
        "file-a",
        &response.files["file-a"],
        &corrupted,
    )
    .await;
    assert_status(result, 422);

    upload_bytes(
        &client,
        server.port,
        &response.session_id,
        "file-a",
        &response.files["file-a"],
        &bytes,
    )
    .await
    .unwrap();

    // The corrupted attempt has been overwritten, not kept as a second file.
    let mut entries = tokio::fs::read_dir(&save_dir).await.unwrap();
    let mut file_names = Vec::new();
    while let Some(entry) = entries.next_entry().await.unwrap() {
        file_names.push(entry.file_name().to_string_lossy().to_string());
    }
    assert_eq!(file_names, vec!["file-a".to_string()]);
    assert_eq!(
        tokio::fs::read(save_dir.join("file-a")).await.unwrap(),
        bytes
    );

    tokio::fs::remove_dir_all(&save_dir).await.unwrap();
}

#[tokio::test]
async fn test_upload_mismatched_sha256_attempts_exhausted() {
    let server = start_test_server(None, true, None).await;
    let client = LsHttpClientV2::try_new_without_cert().unwrap();

    let bytes = b"hello".to_vec();
    let corrupted = b"hellO".to_vec();
    let mut file = file_dto("file-a", "a.bin", bytes.len() as u64);
    file.sha256 = Some(sha256_hex(&bytes));

    let response = client
        .prepare_upload(
            ProtocolType::Http,
            "127.0.0.1",
            server.port,
            None,
            prepare_upload_request(&[file]),
            None,
            CancellationToken::new(),
        )
        .await
        .unwrap()
        .response
        .unwrap();

    // Three attempts are allowed, each failing with a checksum mismatch.
    for _ in 0..3 {
        let result = upload_bytes(
            &client,
            server.port,
            &response.session_id,
            "file-a",
            &response.files["file-a"],
            &corrupted,
        )
        .await;
        assert_status(result, 422);
    }

    // The file is now failed and the session is over; further attempts are rejected.
    let result = upload_bytes(
        &client,
        server.port,
        &response.session_id,
        "file-a",
        &response.files["file-a"],
        &bytes,
    )
    .await;
    assert_status(result, 403);

    tokio::time::sleep(Duration::from_millis(100)).await;
    let session_ends = server.session_ends.lock().await;
    assert_eq!(session_ends.len(), 1);
}

#[tokio::test]
async fn test_upload_saved_to_path_by_server() {
    let save_dir = std::env::temp_dir().join(format!("localsend-test-{}", uuid::Uuid::new_v4()));
    tokio::fs::create_dir_all(&save_dir).await.unwrap();

    let server = start_test_server(None, true, Some(save_dir.clone())).await;
    let client = LsHttpClientV2::try_new_without_cert().unwrap();

    let file_a = file_dto("file-a", "a.bin", 100_000);
    let file_b = file_dto("file-b", "b.bin", 5);

    let result = client
        .prepare_upload(
            ProtocolType::Http,
            "127.0.0.1",
            server.port,
            None,
            prepare_upload_request(&[file_a.clone(), file_b.clone()]),
            None,
            CancellationToken::new(),
        )
        .await
        .unwrap();
    let response = result.response.unwrap();

    let bytes_a: Vec<u8> = (0..100_000u32).map(|i| i as u8).collect();
    let bytes_b = b"hello".to_vec();

    upload_bytes(
        &client,
        server.port,
        &response.session_id,
        "file-a",
        &response.files["file-a"],
        &bytes_a,
    )
    .await
    .unwrap();

    upload_bytes(
        &client,
        server.port,
        &response.session_id,
        "file-b",
        &response.files["file-b"],
        &bytes_b,
    )
    .await
    .unwrap();

    // The test harness reads the files back after the server reported the result.
    tokio::time::sleep(Duration::from_millis(100)).await;
    let received = server.received.lock().await;
    assert_eq!(received["file-a"], bytes_a);
    assert_eq!(received["file-b"], bytes_b);
    drop(received);

    let session_ends = server.session_ends.lock().await;
    assert_eq!(
        *session_ends,
        vec![(response.session_id.clone(), SessionEndReasonV2::Finished)]
    );

    let _ = tokio::fs::remove_dir_all(&save_dir).await;
}

#[tokio::test]
async fn receiver_cancel_removes_only_the_incomplete_staged_file() {
    let save_dir = std::env::temp_dir().join(format!("localsend-test-{}", uuid::Uuid::new_v4()));
    tokio::fs::create_dir(&save_dir).await.unwrap();
    let server =
        start_test_server_with_verification(None, true, Some(save_dir.clone()), true, true).await;
    let client = LsHttpClientV2::try_new_without_cert().unwrap();
    let response = client
        .prepare_upload(
            ProtocolType::Http,
            "127.0.0.1",
            server.port,
            None,
            prepare_upload_request(&[
                file_dto("complete", "complete.bin", 5),
                file_dto("partial", "partial.bin", 5),
            ]),
            None,
            CancellationToken::new(),
        )
        .await
        .unwrap()
        .response
        .unwrap();
    upload_bytes(
        &client,
        server.port,
        &response.session_id,
        "complete",
        &response.files["complete"],
        b"hello",
    )
    .await
    .unwrap();

    let (chunks, receiver) = mpsc::channel(1);
    chunks.send(Bytes::from_static(b"h")).await.unwrap();
    let session_id = response.session_id.clone();
    let port = server.port;
    let partial_path = save_dir.join("partial");
    let upload = tokio::spawn(async move {
        client
            .upload(
                ProtocolType::Http,
                "127.0.0.1",
                port,
                None,
                &response.session_id,
                "partial",
                &response.files["partial"],
                localsend::reqwest::Body::wrap_stream(
                    ReceiverStream::new(receiver).map(Ok::<_, std::io::Error>),
                ),
                CancellationToken::new(),
            )
            .await
    });
    tokio::time::timeout(Duration::from_secs(5), async {
        while !partial_path.exists() {
            tokio::time::sleep(Duration::from_millis(10)).await;
        }
    })
    .await
    .unwrap();
    assert!(!server.handle.cancel_v2_session("other-session").await);
    assert!(server.handle.cancel_v2_session(&session_id).await);
    let result = tokio::time::timeout(Duration::from_secs(5), upload)
        .await
        .expect("cancel should end the upload while the request body is still open")
        .unwrap();
    assert_status(result, 410);
    drop(chunks);
    assert!(!partial_path.exists());
    assert_eq!(tokio::fs::read(save_dir.join("complete")).await.unwrap(), b"hello");
    tokio::fs::remove_file(save_dir.join("complete")).await.unwrap();
    tokio::fs::remove_dir(&save_dir).await.unwrap();
}

/// A source file the sender cannot read must fail the upload instead of being
/// sent as an empty body.
///
/// The receiver creates the destination before the first byte arrives, so an
/// upload that silently ends without data leaves a 0-byte file behind while
/// the sender streamed a well-formed, complete-looking request.
#[tokio::test]
async fn test_upload_of_unreadable_file_fails_instead_of_sending_an_empty_body() {
    let save_dir = std::env::temp_dir().join(format!("localsend-test-{}", uuid::Uuid::new_v4()));
    tokio::fs::create_dir_all(&save_dir).await.unwrap();

    let server = start_test_server(None, true, Some(save_dir.clone())).await;
    let client = LsHttpClientV2::try_new_without_cert().unwrap();

    let file = file_dto("file-a", "a.bin", 5);
    let result = client
        .prepare_upload(
            ProtocolType::Http,
            "127.0.0.1",
            server.port,
            None,
            prepare_upload_request(&[file]),
            None,
            CancellationToken::new(),
        )
        .await
        .unwrap();
    let response = result.response.unwrap();

    // The source is gone by the time the upload starts, as an OS-managed
    // temporary copy of a picked file can be.
    let missing = save_dir.join("source-is-gone.bin");
    let result = LsHttpClient::V2(client)
        .upload(
            ProtocolType::Http,
            "127.0.0.1",
            server.port,
            None,
            &response.session_id,
            "file-a",
            &response.files["file-a"],
            FileContent::Path(missing),
            |_| {},
            CancellationToken::new(),
        )
        .await;

    // The sender has to fail on its own. A status code here would mean the
    // empty body was sent and only the receiver objected, which is exactly the
    // behaviour this fixes - and it would still be an `Err`, so asserting
    // `is_err()` alone would not catch a regression.
    match result {
        Err(ClientError::StatusCode(err)) => panic!(
            "the empty body reached the receiver, which rejected it with {}; \
             the sender should have failed before sending anything",
            err.status
        ),
        Ok(()) => panic!("an unreadable source must not produce a successful upload"),
        Err(_) => {}
    }

    // The receiver must not have stored the file as completely received.
    tokio::time::sleep(Duration::from_millis(100)).await;
    assert!(!server.received.lock().await.contains_key("file-a"));

    let _ = tokio::fs::remove_dir_all(&save_dir).await;
}

#[tokio::test]
async fn test_upload_with_invalid_token() {
    let server = start_test_server(None, true, None).await;
    let client = LsHttpClientV2::try_new_without_cert().unwrap();

    let file = file_dto("file-a", "a.bin", 5);
    let response = client
        .prepare_upload(
            ProtocolType::Http,
            "127.0.0.1",
            server.port,
            None,
            prepare_upload_request(&[file]),
            None,
            CancellationToken::new(),
        )
        .await
        .unwrap()
        .response
        .unwrap();

    let result = upload_bytes(
        &client,
        server.port,
        &response.session_id,
        "file-a",
        "wrong-token",
        b"hello",
    )
    .await;
    assert_status(result, 403);

    // The correct token still works afterwards.
    upload_bytes(
        &client,
        server.port,
        &response.session_id,
        "file-a",
        &response.files["file-a"],
        b"hello",
    )
    .await
    .unwrap();
}

#[tokio::test]
async fn test_upload_missing_parameters() {
    let server = start_test_server(None, true, None).await;

    let response = localsend::reqwest::Client::new()
        .post(format!(
            "http://127.0.0.1:{}/api/localsend/v2/upload?sessionId=abc",
            server.port
        ))
        .body("data")
        .send()
        .await
        .unwrap();
    assert_eq!(response.status().as_u16(), 400);
}

#[tokio::test]
async fn test_second_session_blocked_and_cancel() {
    let server = start_test_server(None, true, None).await;
    let client = LsHttpClientV2::try_new_without_cert().unwrap();

    let file = file_dto("file-a", "a.bin", 5);
    let response = client
        .prepare_upload(
            ProtocolType::Http,
            "127.0.0.1",
            server.port,
            None,
            prepare_upload_request(&[file.clone()]),
            None,
            CancellationToken::new(),
        )
        .await
        .unwrap()
        .response
        .unwrap();

    // A second session is blocked while the first one is active.
    let result = client
        .prepare_upload(
            ProtocolType::Http,
            "127.0.0.1",
            server.port,
            None,
            prepare_upload_request(&[file.clone()]),
            None,
            CancellationToken::new(),
        )
        .await;
    assert_status(result, 409);

    client
        .cancel(
            ProtocolType::Http,
            "127.0.0.1",
            server.port,
            &response.session_id,
        )
        .await
        .unwrap();

    tokio::time::sleep(Duration::from_millis(100)).await;
    let session_ends = server.session_ends.lock().await.clone();
    assert_eq!(
        session_ends,
        vec![(response.session_id.clone(), SessionEndReasonV2::Cancelled)]
    );

    // After cancelling, a new session can be created.
    client
        .prepare_upload(
            ProtocolType::Http,
            "127.0.0.1",
            server.port,
            None,
            prepare_upload_request(&[file]),
            None,
            CancellationToken::new(),
        )
        .await
        .unwrap();
}

#[tokio::test]
async fn busy_receiver_preserves_unused_token() {
    let server = start_test_server(None, true, None).await;
    server.handle.enable_v2_token_gate().await;
    server.handle.grant_v2_token("first-token".into()).await;
    server.handle.grant_v2_token("second-token".into()).await;
    let client = LsHttpClientV2::try_new_without_cert().unwrap();
    let prepare = |token| client.prepare_upload(
        ProtocolType::Http, "127.0.0.1", server.port, None,
        prepare_upload_request(&[file_dto("file-a", "a.txt", 5)]), Some(token), CancellationToken::new(),
    );

    let first = prepare("first-token").await.unwrap().response.unwrap();
    assert_status(prepare("second-token").await, 409);
    assert_status(prepare("first-token").await, 401);
    client.cancel(ProtocolType::Http, "127.0.0.1", server.port, &first.session_id).await.unwrap();

    assert!(prepare("second-token").await.unwrap().response.is_some());
    assert_status(prepare("second-token").await, 401);
}

/// The sender aborts the prepare-upload request (drops the connection) while
/// the receiving application is still deciding. The application must be told
/// via [ServerEventV2::PrepareUploadAborted] and the session slot must be
/// freed so the next request is not blocked.
#[tokio::test]
async fn test_prepare_upload_aborted_by_sender_disconnect() {
    let _ = tracing_subscriber::fmt().with_test_writer().try_init();

    let (event_tx, mut event_rx) = mpsc::channel::<ServerEventV2>(16);
    let (aborted_tx, aborted_rx) = oneshot::channel::<String>();

    // Unlike the shared harness, this event loop does not answer the first
    // prepare-upload decision: the request stays pending like a real
    // application waiting for user input. Later requests are declined so the
    // test can verify the slot was freed without hanging.
    tokio::spawn(async move {
        let mut held_decision = None;
        let mut aborted_tx = Some(aborted_tx);
        while let Some(event) = event_rx.recv().await {
            match event {
                ServerEventV2::PrepareUpload { decision_tx, .. } => {
                    if held_decision.is_none() {
                        held_decision = Some(decision_tx);
                    } else {
                        let _ = decision_tx.send(PrepareUploadDecisionV2::Decline);
                    }
                }
                ServerEventV2::PrepareUploadAborted { session_id } => {
                    if let Some(tx) = aborted_tx.take() {
                        let _ = tx.send(session_id);
                    }
                }
                _ => {}
            }
        }
    });

    let (stop_tx, stop_rx) = oneshot::channel::<()>();
    let handle = start_with_port(
        0,
        None, // plain HTTP
        ClientInfo {
            alias: "Test Server".to_string(),
            version: "2.2".to_string(),
            device_model: Some("Rust".to_string()),
            device_type: None,
            token: "server-fingerprint".to_string(),
        },
        None,
        Some(ServerConfigV2 {
            pin: None,
            verify_checksums: true,
            event_tx,
        }),
        WebConfig::default(),
        stop_rx,
    )
    .await
    .expect("Failed to start server");
    let port = handle.port();

    // Raw TCP so the connection can be closed mid-request.
    let body =
        serde_json::to_string(&prepare_upload_request(&[file_dto("file-a", "a.bin", 5)])).unwrap();
    let request = format!(
        "POST /api/localsend/v2/prepare-upload HTTP/1.1\r\nHost: 127.0.0.1:{port}\r\nContent-Type: application/json\r\nContent-Length: {}\r\n\r\n{body}",
        body.len()
    );
    let mut stream = tokio::net::TcpStream::connect(("127.0.0.1", port))
        .await
        .unwrap();
    tokio::io::AsyncWriteExt::write_all(&mut stream, request.as_bytes())
        .await
        .unwrap();

    // Give the server time to read the request and emit PrepareUpload,
    // then hang up without waiting for the response.
    tokio::time::sleep(Duration::from_millis(200)).await;
    drop(stream);

    tokio::time::timeout(Duration::from_secs(3), aborted_rx)
        .await
        .expect("PrepareUploadAborted was not emitted after the sender disconnected")
        .unwrap();

    // The pending slot must be free again for the next sender.
    let client = LsHttpClientV2::try_new_without_cert().unwrap();
    let result = client
        .prepare_upload(
            ProtocolType::Http,
            "127.0.0.1",
            port,
            None,
            prepare_upload_request(&[file_dto("file-b", "b.bin", 5)]),
            None,
            CancellationToken::new(),
        )
        .await;
    // The event loop declines the second request; 409 would mean the
    // pending slot of the aborted request leaked.
    assert_status(result, 403);

    let _ = stop_tx.send(());
}

/// A released (Dart) sender cancels a pending prepare-upload with a
/// session-less `POST /cancel` while keeping the prepare-upload request open:
/// it does not know the session ID (that is part of the response it never
/// waits for) and does not abort the connection. The pending request must be
/// rejected, the application notified, and the slot freed.
#[tokio::test]
async fn test_prepare_upload_cancelled_by_session_less_cancel() {
    let _ = tracing_subscriber::fmt().with_test_writer().try_init();

    let (event_tx, mut event_rx) = mpsc::channel::<ServerEventV2>(16);
    let (aborted_tx, mut aborted_rx) = oneshot::channel::<String>();

    tokio::spawn(async move {
        let mut held_decision = None;
        let mut aborted_tx = Some(aborted_tx);
        while let Some(event) = event_rx.recv().await {
            match event {
                ServerEventV2::PrepareUpload { decision_tx, .. } => {
                    if held_decision.is_none() {
                        held_decision = Some(decision_tx);
                    } else {
                        let _ = decision_tx.send(PrepareUploadDecisionV2::Decline);
                    }
                }
                ServerEventV2::PrepareUploadAborted { session_id } => {
                    if let Some(tx) = aborted_tx.take() {
                        let _ = tx.send(session_id);
                    }
                }
                _ => {}
            }
        }
    });

    let (stop_tx, stop_rx) = oneshot::channel::<()>();
    let handle = start_with_port(
        0,
        None, // plain HTTP
        ClientInfo {
            alias: "Test Server".to_string(),
            version: "2.2".to_string(),
            device_model: Some("Rust".to_string()),
            device_type: None,
            token: "server-fingerprint".to_string(),
        },
        None,
        Some(ServerConfigV2 {
            pin: None,
            verify_checksums: true,
            event_tx,
        }),
        WebConfig::default(),
        stop_rx,
    )
    .await
    .expect("Failed to start server");
    let port = handle.port();

    // The prepare-upload request stays open in the background, like the
    // released sender that fires the cancel without aborting it.
    let prepare_task = tokio::spawn(async move {
        let client = LsHttpClientV2::try_new_without_cert().unwrap();
        client
            .prepare_upload(
                ProtocolType::Http,
                "127.0.0.1",
                port,
                None,
                prepare_upload_request(&[file_dto("file-a", "a.bin", 5)]),
                None,
                CancellationToken::new(),
            )
            .await
    });
    tokio::time::sleep(Duration::from_millis(200)).await;

    let cancel_url = format!("http://127.0.0.1:{port}/api/localsend/v2/cancel");
    let cancel_client = localsend::reqwest::Client::new();

    // A cancel with a wrong session ID must not cancel the pending request.
    cancel_client
        .post(format!("{cancel_url}?sessionId=some-other-session"))
        .send()
        .await
        .unwrap();
    assert!(
        tokio::time::timeout(Duration::from_millis(300), &mut aborted_rx)
            .await
            .is_err(),
        "a cancel with a foreign session ID must not abort the pending request"
    );

    // The session-less cancel of the released sender.
    cancel_client.post(&cancel_url).send().await.unwrap();

    tokio::time::timeout(Duration::from_secs(3), aborted_rx)
        .await
        .expect("PrepareUploadAborted was not emitted after the session-less cancel")
        .unwrap();

    // The open prepare-upload request is answered with a rejection.
    let result = prepare_task.await.unwrap();
    assert_status(result, 403);

    // The pending slot must be free again for the next sender.
    let client = LsHttpClientV2::try_new_without_cert().unwrap();
    let result = client
        .prepare_upload(
            ProtocolType::Http,
            "127.0.0.1",
            port,
            None,
            prepare_upload_request(&[file_dto("file-b", "b.bin", 5)]),
            None,
            CancellationToken::new(),
        )
        .await;
    assert_status(result, 403);

    let _ = stop_tx.send(());
}

/// Same as [test_prepare_upload_aborted_by_sender_disconnect], but over TLS
/// with mutual certificates - the transport every real LocalSend transfer
/// uses - and cancelled through the client's cancellation token, the way a
/// sender cancels while waiting for the receiver's decision.
#[tokio::test]
async fn test_prepare_upload_aborted_by_sender_disconnect_tls() {
    let _ = tracing_subscriber::fmt().with_test_writer().try_init();

    let server_key = rcgen::KeyPair::generate().unwrap();
    let server_cert = rcgen::CertificateParams::new(vec!["LocalSend User".to_string()])
        .unwrap()
        .self_signed(&server_key)
        .unwrap();
    let sender_key = rcgen::KeyPair::generate().unwrap();
    let sender_cert = rcgen::CertificateParams::new(vec!["LocalSend User".to_string()])
        .unwrap()
        .self_signed(&sender_key)
        .unwrap();

    let (event_tx, mut event_rx) = mpsc::channel::<ServerEventV2>(16);
    let (aborted_tx, aborted_rx) = oneshot::channel::<String>();

    tokio::spawn(async move {
        let mut held_decision = None;
        let mut aborted_tx = Some(aborted_tx);
        while let Some(event) = event_rx.recv().await {
            match event {
                ServerEventV2::PrepareUpload { decision_tx, .. } => {
                    if held_decision.is_none() {
                        held_decision = Some(decision_tx);
                    } else {
                        let _ = decision_tx.send(PrepareUploadDecisionV2::Decline);
                    }
                }
                ServerEventV2::PrepareUploadAborted { session_id } => {
                    if let Some(tx) = aborted_tx.take() {
                        let _ = tx.send(session_id);
                    }
                }
                _ => {}
            }
        }
    });

    let (stop_tx, stop_rx) = oneshot::channel::<()>();
    let handle = start_with_port(
        0,
        Some(localsend::http::server::TlsConfig {
            cert: server_cert.pem(),
            private_key: server_key.serialize_pem(),
        }),
        ClientInfo {
            alias: "Test Server".to_string(),
            version: "2.2".to_string(),
            device_model: Some("Rust".to_string()),
            device_type: None,
            token: "server-fingerprint".to_string(),
        },
        None,
        Some(ServerConfigV2 {
            pin: None,
            verify_checksums: true,
            event_tx,
        }),
        WebConfig::default(),
        stop_rx,
    )
    .await
    .expect("Failed to start server");
    let port = handle.port();

    // The sender cancels while the server is still waiting for the
    // application's decision.
    let client =
        LsHttpClientV2::try_new(&sender_key.serialize_pem(), &sender_cert.pem(), None, None)
            .unwrap();
    let cancel = CancellationToken::new();
    tokio::spawn({
        let cancel = cancel.clone();
        async move {
            tokio::time::sleep(Duration::from_millis(300)).await;
            cancel.cancel();
        }
    });
    let result = client
        .prepare_upload(
            ProtocolType::Https,
            "127.0.0.1",
            port,
            None,
            prepare_upload_request(&[file_dto("file-a", "a.bin", 5)]),
            None,
            cancel,
        )
        .await;
    assert!(
        matches!(result, Err(ClientError::Cancelled)),
        "expected ClientError::Cancelled, got {:?}",
        result.err()
    );

    tokio::time::timeout(Duration::from_secs(3), aborted_rx)
        .await
        .expect("PrepareUploadAborted was not emitted after the sender disconnected (TLS)")
        .unwrap();

    // The pending slot must be free again for the next sender.
    let client =
        LsHttpClientV2::try_new(&sender_key.serialize_pem(), &sender_cert.pem(), None, None)
            .unwrap();
    let result = client
        .prepare_upload(
            ProtocolType::Https,
            "127.0.0.1",
            port,
            None,
            prepare_upload_request(&[file_dto("file-b", "b.bin", 5)]),
            None,
            CancellationToken::new(),
        )
        .await;
    assert_status(result, 403);

    let _ = stop_tx.send(());
}

#[tokio::test]
async fn test_prepare_upload_declined() {
    let server = start_test_server(None, false, None).await;
    let client = LsHttpClientV2::try_new_without_cert().unwrap();

    let file = file_dto("file-a", "a.bin", 5);
    let result = client
        .prepare_upload(
            ProtocolType::Http,
            "127.0.0.1",
            server.port,
            None,
            prepare_upload_request(&[file.clone()]),
            None,
            CancellationToken::new(),
        )
        .await;
    assert_status(result, 403);

    // A declined request must not block subsequent sessions.
    let result = client
        .prepare_upload(
            ProtocolType::Http,
            "127.0.0.1",
            server.port,
            None,
            prepare_upload_request(&[file]),
            None,
            CancellationToken::new(),
        )
        .await;
    assert_status(result, 403);
}

/// macOS sends NFD names; the app must only ever see NFC (e.g. Korean as
/// precomposed syllables instead of separate jamo).
#[tokio::test]
async fn test_prepare_upload_normalizes_file_names_to_nfc() {
    let (event_tx, mut event_rx) = mpsc::channel::<ServerEventV2>(16);
    let (_stop_tx, stop_rx) = oneshot::channel::<()>();
    let handle = start_with_port(
        0,
        None,
        ClientInfo {
            alias: "Test Server".to_string(),
            version: "2.2".to_string(),
            device_model: None,
            device_type: None,
            token: "server-fingerprint".to_string(),
        },
        None,
        Some(ServerConfigV2 {
            pin: None,
            verify_checksums: true,
            event_tx,
        }),
        WebConfig::default(),
        stop_rx,
    )
    .await
    .expect("Failed to start server");

    // "오삐한테" as decomposed jamo, inside a folder.
    let nfd = "dir/\u{110B}\u{1169}\u{1108}\u{1175}\u{1112}\u{1161}\u{11AB}\u{1110}\u{1166}.txt";
    let client = LsHttpClientV2::try_new_without_cert().unwrap();
    let request = client.prepare_upload(
        ProtocolType::Http,
        "127.0.0.1",
        handle.port(),
        None,
        prepare_upload_request(&[file_dto("file-a", nfd, 5)]),
        None,
        CancellationToken::new(),
    );

    let observe = async {
        loop {
            if let Some(ServerEventV2::PrepareUpload {
                files, decision_tx, ..
            }) = event_rx.recv().await
            {
                let _ = decision_tx.send(PrepareUploadDecisionV2::Decline);
                return files["file-a"].file_name.clone();
            }
        }
    };

    let (result, file_name) = tokio::join!(request, observe);
    assert_status(result, 403);
    assert_eq!(file_name, "dir/오삐한테.txt");
}

#[tokio::test]
async fn test_pin() {
    let server = start_test_server(Some("123456".to_string()), true, None).await;
    let client = LsHttpClientV2::try_new_without_cert().unwrap();

    let file = file_dto("file-a", "a.bin", 5);

    // Missing PIN.
    let result = client
        .prepare_upload(
            ProtocolType::Http,
            "127.0.0.1",
            server.port,
            None,
            prepare_upload_request(&[file.clone()]),
            None,
            CancellationToken::new(),
        )
        .await;
    assert_status(result, 401);

    // Wrong PIN.
    let result = client
        .prepare_upload(
            ProtocolType::Http,
            "127.0.0.1",
            server.port,
            None,
            prepare_upload_request(&[file.clone()]),
            Some("000000"),
            CancellationToken::new(),
        )
        .await;
    assert_status(result, 401);

    // Correct PIN.
    client
        .prepare_upload(
            ProtocolType::Http,
            "127.0.0.1",
            server.port,
            None,
            prepare_upload_request(&[file]),
            Some("123456"),
            CancellationToken::new(),
        )
        .await
        .unwrap();
}

#[tokio::test]
async fn test_pin_too_many_attempts() {
    let server = start_test_server(Some("123456".to_string()), true, None).await;
    let client = LsHttpClientV2::try_new_without_cert().unwrap();

    let file = file_dto("file-a", "a.bin", 5);

    for _ in 0..3 {
        let result = client
            .prepare_upload(
                ProtocolType::Http,
                "127.0.0.1",
                server.port,
                None,
                prepare_upload_request(&[file.clone()]),
                Some("000000"),
                CancellationToken::new(),
            )
            .await;
        assert_status(result, 401);
    }

    // Blocked even with the correct PIN.
    let result = client
        .prepare_upload(
            ProtocolType::Http,
            "127.0.0.1",
            server.port,
            None,
            prepare_upload_request(&[file]),
            Some("123456"),
            CancellationToken::new(),
        )
        .await;
    assert_status(result, 429);
}

#[tokio::test]
async fn test_prepare_upload_emits_non_secret_pin_tag() {
    let (event_tx, mut event_rx) = mpsc::channel::<ServerEventV2>(16);
    let (_stop_tx, stop_rx) = tokio::sync::oneshot::channel();
    let handle = start_with_port(
        0,
        None,
        ClientInfo {
            alias: "Test Server".to_string(),
            version: "2.2".to_string(),
            device_model: None,
            device_type: None,
            token: "server-fingerprint".to_string(),
        },
        None,
        Some(ServerConfigV2 {
            pin: None,
            verify_checksums: true,
            event_tx,
        }),
        WebConfig::default(),
        stop_rx,
    )
    .await
    .expect("Failed to start server");

    let client = LsHttpClientV2::try_new_without_cert().unwrap();
    let req_no_pin = client.prepare_upload(
        ProtocolType::Http,
        "127.0.0.1",
        handle.port(),
        None,
        prepare_upload_request(&[file_dto("file-a", "a.txt", 5)]),
        None,
        CancellationToken::new(),
    );
    let obs_no_pin = async {
        loop {
            if let Some(ServerEventV2::PrepareUpload {
                pin_tag,
                decision_tx,
                ..
            }) = event_rx.recv().await
            {
                let _ = decision_tx.send(PrepareUploadDecisionV2::Decline);
                return pin_tag;
            }
        }
    };
    let (res1, tag1) = tokio::join!(req_no_pin, obs_no_pin);
    assert_status(res1, 403);
    assert_eq!(tag1, None);

    let secret = "desksend-secret-token-42";
    handle.grant_v2_token(secret.to_string()).await;
    let req_with_pin = client.prepare_upload(
        ProtocolType::Http,
        "127.0.0.1",
        handle.port(),
        None,
        prepare_upload_request(&[file_dto("file-a", "a.txt", 5)]),
        Some(secret),
        CancellationToken::new(),
    );
    let obs_with_pin = async {
        loop {
            if let Some(ServerEventV2::PrepareUpload {
                pin_tag,
                decision_tx,
                ..
            }) = event_rx.recv().await
            {
                let _ = decision_tx.send(PrepareUploadDecisionV2::Decline);
                return pin_tag;
            }
        }
    };
    let (res2, tag2) = tokio::join!(req_with_pin, obs_with_pin);
    assert_status(res2, 403);
    assert_eq!(tag2, Some(sha256_hex(secret.as_bytes())[..16].to_string()));
}
