use crossterm::terminal::{Clear, ClearType};
use crossterm::{cursor, execute};
use localsend::util::filename;
use std::path::{Path, PathBuf};
use std::time::{Duration, Instant};

/// Enters the alternate screen for a modal widget (the file picker or the
/// device list). The caller must suspend the log UI first and resume it
/// after [leave_alternate_screen].
pub fn enter_alternate_screen() -> anyhow::Result<()> {
    execute!(
        std::io::stdout(),
        crossterm::terminal::EnterAlternateScreen,
        cursor::Hide
    )?;
    Ok(())
}

/// Leaves the alternate screen. Must be called exactly once per enter.
///
/// Clears via crossterm, not `Terminal::clear`: the latter queries the
/// cursor position, whose response is read from the event stream but
/// the keyboard reader thread is parked in `crossterm::event::read()`,
/// so the query only ever returns after crossterm's 2s timeout.
pub fn leave_alternate_screen() {
    let _ = execute!(
        std::io::stdout(),
        Clear(ClearType::All),
        cursor::MoveTo(0, 0),
        crossterm::terminal::LeaveAlternateScreen,
        cursor::Show
    );
}

/// Clears the alternate screen without leaving it, for handing it over from
/// one modal to the next — leaving and re-entering would flash the main
/// screen in between.
pub fn clear_alternate_screen() {
    let _ = execute!(
        std::io::stdout(),
        Clear(ClearType::All),
        cursor::MoveTo(0, 0)
    );
}

pub fn format_bytes(bytes: u64) -> String {
    const UNITS: [&str; 5] = ["B", "KB", "MB", "GB", "TB"];
    let mut value = bytes as f64;
    let mut unit = 0;
    while value >= 1000.0 && unit < UNITS.len() - 1 {
        value /= 1000.0;
        unit += 1;
    }
    match unit {
        0 => format!("{bytes} B"),
        _ => format!("{value:.1} {}", UNITS[unit]),
    }
}

pub fn format_speed(bytes_per_sec: f64) -> String {
    format!("{}/s", format_bytes(bytes_per_sec.max(0.0) as u64))
}

pub fn format_duration(duration: Duration) -> String {
    let secs = duration.as_secs();
    let (h, m, s) = (secs / 3600, (secs % 3600) / 60, secs % 60);
    if h > 0 {
        format!("{h}h {m}m")
    } else if m > 0 {
        format!("{m}m {s}s")
    } else {
        format!("{s}s")
    }
}

/// The width of the terminal in columns, falling back to 120 when it cannot be
/// determined (e.g. when the output is not a terminal).
pub fn terminal_width() -> usize {
    crossterm::terminal::size()
        .map(|(w, _)| w as usize)
        .unwrap_or(120)
}

pub fn progress_bar(fraction: f64, width: usize) -> String {
    let filled = (fraction.clamp(0.0, 1.0) * width as f64).round() as usize;
    format!("{}{}", "#".repeat(filled), "-".repeat(width - filled))
}

/// A path in `dir` for `file_name` that does not exist yet, appending
/// ` (1)`, ` (2)`, … before the extension on collisions.
///
/// `file_name` comes from the sender and is untrusted: it is collapsed to its
/// final path component and sanitized for the local filesystem, so it can
/// neither escape the target directory nor carry illegal characters.
pub fn unique_path(dir: &Path, file_name: &str) -> PathBuf {
    let _ = std::fs::create_dir_all(dir);
    let name = filename::sanitize_path(file_name, filename::Rules::current());

    unique_sanitized_path(dir, &name)
}

fn unique_sanitized_path(dir: &Path, name: &str) -> PathBuf {
    let candidate = dir.join(name);
    if std::fs::symlink_metadata(&candidate).is_err() {
        return candidate;
    }

    let (stem, extension) = match name.rsplit_once('.') {
        Some((stem, extension)) if !stem.is_empty() => (stem, format!(".{extension}")),
        _ => (name, String::new()),
    };
    (1..)
        .map(|i| dir.join(format!("{stem} ({i}){extension}")))
        .find(|candidate| std::fs::symlink_metadata(candidate).is_err())
        .unwrap()
}

/// Resolves a sender-supplied relative path (e.g. `MyFolder/sub/file.txt`) inside `dir`,
/// sanitizing each path component individually and stripping any `.` or `..` segments
/// so folder hierarchy is preserved safely without path traversal.
pub fn unique_nested_path(dir: &Path, file_name: &str, preserve_line_breaks: bool) -> std::io::Result<PathBuf> {
    ensure_directory_chain(dir)?;
    let rules = filename::Rules::current();
    let segments: Vec<String> = file_name
        .split(['/', '\\'])
        .filter(|seg| !seg.is_empty() && *seg != "." && *seg != "..")
        .map(|seg| {
            if preserve_line_breaks
                && matches!(rules, filename::Rules::Posix | filename::Rules::Hfs)
                && seg.contains(['\r', '\n'])
            {
                let normalized = filename::normalize(seg);
                let mut name = String::new();
                for ch in normalized.chars() {
                    let sanitized = match ch {
                        '\r' | '\n' => ch,
                        ch if ch.is_control() || (ch == ':' && rules == filename::Rules::Hfs) => '_',
                        ch => ch,
                    };
                    if name.len() + sanitized.len_utf8() > 255 {
                        break;
                    }
                    name.push(sanitized);
                }
                name
            } else {
                filename::sanitize(seg, rules)
            }
        })
        .filter(|seg| !seg.is_empty() && seg != "." && seg != "..")
        .collect();

    if segments.is_empty() {
        return Ok(unique_path(dir, file_name));
    }

    let mut parent = dir.to_path_buf();
    for seg in &segments[..segments.len() - 1] {
        parent.push(seg);
        ensure_directory_chain(&parent)?;
    }

    let last = &segments[segments.len() - 1];
    Ok(unique_sanitized_path(&parent, last))
}

fn ensure_directory_chain(dir: &Path) -> std::io::Result<()> {
    let mut ancestors: Vec<_> = dir.ancestors().filter(|path| !path.as_os_str().is_empty()).collect();
    while let Some(path) = ancestors.pop() {
        if std::fs::symlink_metadata(path).is_err() {
            localsend::http::server::common::save::check_receive_path(path)?;
            std::fs::create_dir(path)?;
        }
        localsend::http::server::common::save::check_receive_path(path)?;
        // 路径检查已拒绝普通链接，这里检查获准的系统链接所指向的目录。
        if !std::fs::metadata(path)?.is_dir() {
            return Err(std::io::Error::other("Receive parent is not a directory"));
        }
    }
    Ok(())
}

pub fn read_bounded_line(input: &mut impl std::io::BufRead, limit: usize) -> std::io::Result<Option<String>> {
    use std::io::{BufRead, Read};
    let mut bytes = Vec::new();
    let count = input.take((limit + 2) as u64).read_until(b'\n', &mut bytes)?;
    if count == 0 {
        return Ok(None);
    }
    if bytes.last() == Some(&b'\n') {
        bytes.pop();
        if bytes.last() == Some(&b'\r') {
            bytes.pop();
        }
    }
    if bytes.len() > limit {
        return Err(std::io::Error::other("stdin line exceeds limit"));
    }
    String::from_utf8(bytes).map(Some).map_err(std::io::Error::other)
}

/// Estimates the transfer speed from cumulative byte counts, smoothed with an
/// exponential moving average.
pub struct SpeedMeter {
    last_bytes: u64,
    last_time: Instant,
    ema: f64,
}

impl SpeedMeter {
    pub fn new() -> Self {
        Self {
            last_bytes: 0,
            last_time: Instant::now(),
            ema: 0.0,
        }
    }

    pub fn update(&mut self, bytes_now: u64) -> f64 {
        let now = Instant::now();
        let dt = now.duration_since(self.last_time).as_secs_f64();
        if dt < 0.1 {
            return self.ema;
        }
        let instantaneous = bytes_now.saturating_sub(self.last_bytes) as f64 / dt;
        self.ema = match self.ema {
            0.0 => instantaneous,
            ema => ema * 0.7 + instantaneous * 0.3,
        };
        self.last_bytes = bytes_now;
        self.last_time = now;
        self.ema
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[cfg(unix)]
    #[test]
    fn nested_receive_rejects_symlink_parent() {
        let base = std::env::temp_dir().join(format!("localsend-link-test-{}", std::process::id()));
        std::fs::create_dir_all(base.join("outside")).unwrap();
        std::fs::create_dir_all(base.join("receive")).unwrap();
        std::os::unix::fs::symlink(base.join("outside"), base.join("receive/link")).unwrap();
        assert!(unique_nested_path(&base.join("receive"), "link/sub/file", false).is_err());
        assert!(!base.join("outside/sub").exists());
        std::fs::remove_dir_all(base).unwrap();
    }

    #[test]
    fn stdin_line_limit_rejects_oversized_input() {
        let mut input = std::io::Cursor::new(b"1234\r\n12345\n");
        assert_eq!(read_bounded_line(&mut input, 4).unwrap().as_deref(), Some("1234"));
        assert!(read_bounded_line(&mut input, 4).is_err());
    }

    #[test]
    fn unique_nested_path_preserves_subdirectories_and_strips_traversal() {
        let base = std::env::temp_dir().join(format!(
            "localsend_nested_test_{}",
            std::process::id()
        ));
        let _ = std::fs::remove_dir_all(&base);
        std::fs::create_dir_all(&base).unwrap();

        let p1 = unique_nested_path(&base, "MyFolder/sub/hello.txt", false).unwrap();
        assert_eq!(p1, base.join("MyFolder").join("sub").join("hello.txt"));
        assert!(base.join("MyFolder").join("sub").is_dir());

        let p2 = unique_nested_path(&base, "../../EvilFolder/../safe.txt", false).unwrap();
        assert_eq!(p2, base.join("EvilFolder").join("safe.txt"));

        let _ = std::fs::remove_dir_all(&base);
    }
}

/// 解码 DeskSend 的 UTF-8 路径帧，拒绝空路径、NUL 及畸形十六进制。
pub fn decode_path_hex(frame: &str) -> anyhow::Result<String> {
    anyhow::ensure!(!frame.is_empty() && frame.len() % 2 == 0, "Invalid path frame");
    let mut bytes = Vec::with_capacity(frame.len() / 2);
    for pair in frame.as_bytes().chunks_exact(2) {
        let hi = (pair[0] as char).to_digit(16).ok_or_else(|| anyhow::anyhow!("Invalid path frame"))?;
        let lo = (pair[1] as char).to_digit(16).ok_or_else(|| anyhow::anyhow!("Invalid path frame"))?;
        bytes.push(((hi << 4) | lo) as u8);
    }
    anyhow::ensure!(!bytes.contains(&0), "NUL in path frame");
    Ok(String::from_utf8(bytes)?)
}

pub fn encode_path_hex(path: &str) -> String {
    use std::fmt::Write;
    let mut frame = String::with_capacity(path.len() * 2);
    for byte in path.bytes() {
        let _ = write!(frame, "{byte:02x}");
    }
    frame
}

#[cfg(test)]
mod path_frame_tests {
    use super::{decode_path_hex, encode_path_hex};

    #[test]
    fn preserves_cr_lf_and_unicode() {
        let path = "folder/a\r\n中文.txt";
        assert_eq!(decode_path_hex(&encode_path_hex(path)).unwrap(), path);
    }

    #[test]
    fn rejects_invalid_paths() {
        for frame in ["", "0", "zz", "ff", "00"] {
            assert!(decode_path_hex(frame).is_err());
        }
    }
}
