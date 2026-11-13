use crate::http::server::common::error::AppError;
use hyper::StatusCode;
use lru::LruCache;
use std::collections::HashMap;
use std::net::IpAddr;
use std::time::{Duration, Instant};
use tokio::sync::Mutex;

/// Maximum failed PIN attempts per IP before requests are blocked with 429.
const MAX_PIN_ATTEMPTS: u32 = 3;

/// Maximum lifetime of an unused one-time transfer authorization token.
const TOKEN_TTL: Duration = Duration::from_secs(300);

/// Maximum number of concurrent pending authorization tokens.
const MAX_PENDING_TOKENS: usize = 64;

/// Compares two byte slices in constant time when they share the same length.
pub(crate) fn constant_time_eq(a: &[u8], b: &[u8]) -> bool {
    if a.len() != b.len() {
        return false;
    }
    let mut diff = 0u8;
    for (&x, &y) in a.iter().zip(b.iter()) {
        diff |= x ^ y;
    }
    diff == 0
}

/// Optional one-time token gate enforced on v2 upload preparation when enabled.
#[derive(Default)]
pub(crate) struct TokenGate {
    enabled: bool,
    tokens: HashMap<String, Instant>,
}

impl TokenGate {
    pub(crate) fn enable(&mut self) {
        self.enabled = true;
    }

    pub(crate) fn is_enabled(&self) -> bool {
        self.enabled
    }

    fn purge_expired(&mut self, now: Instant) {
        self.tokens.retain(|_, exp| *exp > now);
    }

    pub(crate) fn grant(&mut self, token: String) {
        self.grant_at(token, Instant::now());
    }

    pub(crate) fn revoke(&mut self, token: &str) -> bool {
        self.revoke_at(token, Instant::now())
    }

    pub(crate) fn verify_and_consume(&mut self, candidate: &str) -> bool {
        self.verify_and_consume_at(candidate, Instant::now())
    }

    fn grant_at(&mut self, token: String, now: Instant) {
        if token.is_empty() {
            return;
        }
        self.purge_expired(now);
        if !self.tokens.contains_key(&token) && self.tokens.len() >= MAX_PENDING_TOKENS {
            if let Some(oldest) = self
                .tokens
                .iter()
                .min_by_key(|(_, exp)| *exp)
                .map(|(k, _)| k.clone())
            {
                self.tokens.remove(&oldest);
            }
        }
        self.tokens.insert(token, now + TOKEN_TTL);
    }

    fn revoke_at(&mut self, token: &str, now: Instant) -> bool {
        self.purge_expired(now);
        let mut matched: Option<String> = None;
        for k in self.tokens.keys() {
            if constant_time_eq(k.as_bytes(), token.as_bytes()) {
                matched = Some(k.clone());
            }
        }
        if let Some(k) = matched {
            self.tokens.remove(&k);
            true
        } else {
            false
        }
    }

    fn verify_and_consume_at(&mut self, candidate: &str, now: Instant) -> bool {
        self.purge_expired(now);
        let mut matched: Option<String> = None;
        for k in self.tokens.keys() {
            if constant_time_eq(k.as_bytes(), candidate.as_bytes()) {
                matched = Some(k.clone());
            }
        }
        if let Some(k) = matched {
            self.tokens.remove(&k);
            true
        } else {
            false
        }
    }
}

/// Checks the `pin` query parameter against the required PIN (if any).
pub(crate) async fn check_pin(
    required_pin: Option<&str>,
    pin_attempts: &Mutex<LruCache<IpAddr, u32>>,
    query: &HashMap<String, String>,
    ip: IpAddr,
) -> Result<(), AppError> {
    let Some(required_pin) = required_pin else {
        return Ok(());
    };

    let mut attempts = pin_attempts.lock().await;
    let count = attempts.get(&ip).copied().unwrap_or(0);
    if count >= MAX_PIN_ATTEMPTS {
        return Err(AppError::Message(
            StatusCode::TOO_MANY_REQUESTS,
            "Too many requests".to_string(),
        ));
    }

    match query.get("pin") {
        Some(pin) if constant_time_eq(pin.as_bytes(), required_pin.as_bytes()) => {
            attempts.pop(&ip);
            Ok(())
        }
        Some(_) => {
            attempts.put(ip, count + 1);
            Err(AppError::Message(
                StatusCode::UNAUTHORIZED,
                "Invalid PIN".to_string(),
            ))
        }
        None => Err(AppError::Message(
            StatusCode::UNAUTHORIZED,
            "PIN required".to_string(),
        )),
    }
}

/// Checks and consumes a one-time authorization token from the `pin` query parameter
/// when [TokenGate] is enabled.
pub(crate) async fn check_token_gate(
    token_gate: &Mutex<TokenGate>,
    pin_attempts: &Mutex<LruCache<IpAddr, u32>>,
    query: &HashMap<String, String>,
    ip: IpAddr,
) -> Result<(), AppError> {
    {
        let gate = token_gate.lock().await;
        if !gate.is_enabled() {
            return Ok(());
        }
    }

    {
        let mut attempts = pin_attempts.lock().await;
        let count = attempts.get(&ip).copied().unwrap_or(0);
        if count >= MAX_PIN_ATTEMPTS {
            return Err(AppError::Message(
                StatusCode::TOO_MANY_REQUESTS,
                "Too many requests".to_string(),
            ));
        }
    }

    let Some(provided_pin) = query.get("pin").filter(|p| !p.is_empty()) else {
        return Err(AppError::Message(
            StatusCode::UNAUTHORIZED,
            "PIN required".to_string(),
        ));
    };

    let accepted = {
        let mut gate = token_gate.lock().await;
        gate.verify_and_consume(provided_pin)
    };

    let mut attempts = pin_attempts.lock().await;
    if accepted {
        attempts.pop(&ip);
        Ok(())
    } else {
        let count = attempts.get(&ip).copied().unwrap_or(0);
        attempts.put(ip, count + 1);
        Err(AppError::Message(
            StatusCode::UNAUTHORIZED,
            "Invalid PIN".to_string(),
        ))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn token_gate_grant_consume_and_replay_rejected() {
        let now = Instant::now();
        let mut gate = TokenGate::default();
        gate.enable();
        gate.grant_at("secret-token-1".to_string(), now);

        assert!(!gate.verify_and_consume_at("wrong-token", now));
        assert!(gate.verify_and_consume_at("secret-token-1", now));
        // Replay must fail because token was consumed on first use.
        assert!(!gate.verify_and_consume_at("secret-token-1", now));
    }

    #[test]
    fn token_gate_revoke_and_expiry() {
        let now = Instant::now();
        let mut gate = TokenGate::default();
        gate.enable();
        gate.grant_at("t1".to_string(), now);
        gate.grant_at("t2".to_string(), now);

        assert!(gate.revoke_at("t1", now));
        assert!(!gate.revoke_at("t1", now));
        assert!(!gate.verify_and_consume_at("t1", now));

        assert!(!gate.verify_and_consume_at("t2", now + TOKEN_TTL + Duration::from_secs(1)));
    }
}
