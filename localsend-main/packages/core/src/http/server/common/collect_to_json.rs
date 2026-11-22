use crate::http::server::common::error::AppError;
use http_body_util::BodyExt;
use hyper::body::Incoming;
use serde::de::DeserializeOwned;

pub(crate) trait CollectToJson {
    async fn collect_to_json<T: DeserializeOwned>(self, limit: usize) -> Result<T, AppError>;
}

impl CollectToJson for Incoming {
    async fn collect_to_json<T: DeserializeOwned>(mut self, limit: usize) -> Result<T, AppError> {
        let mut bytes = Vec::new();
        while let Some(frame) = self.frame().await {
            if let Ok(data) = frame?.into_data() {
                if data.len() > limit - bytes.len() {
                    return Err(AppError::Status(hyper::StatusCode::PAYLOAD_TOO_LARGE));
                }
                bytes.extend_from_slice(&data);
            }
        }
        serde_json::from_slice::<T>(&bytes).map_err(|err| {
            tracing::warn!("Failed to parse JSON body: {err:#}");
            AppError::BadRequest("Invalid JSON body".to_string())
        })
    }
}
