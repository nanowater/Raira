//! Lyrics lookup on LRCLIB (https://lrclib.net), first written for the device in C++ and moved here (same rules): the PC has
//! the TLS stack, the CPU and a place for a cache; the device only receives the finished line list. Candidates are the
//! exact lookup plus a free-text search, ranked by length, synced or not, script, album and variant tags.

use std::path::PathBuf;
use std::sync::OnceLock;
use std::time::Duration;

use serde::{Deserialize, Serialize};
use serde_json::Value;
use tokio::sync::Semaphore;

const BASE: &str = "https://lrclib.net/api/";

/// LRCLIB's address; `RAIRA_LRCLIB` replaces it (tests against a fake server).
fn base() -> String {
    std::env::var("RAIRA_LRCLIB").unwrap_or_else(|_| BASE.to_string())
}
/// A message to the device may be 64 KB at most (docs/HUB_PROTOCOL.md): longer lyrics are cut.
pub const MAX_LINES_BYTES: usize = 60 * 1024;
/// How long "LRCLIB has nothing" is believed. Short on purpose: it also comes out when LRCLIB was only briefly unwell, and a week of
/// believing that meant a song without lyrics for a week.
const NEGATIVE_TTL: Duration = Duration::from_secs(3600);
/// A lookup that came back empty or failed is repeated once after this pause before it is believed.
const RETRY_AFTER: Duration = Duration::from_secs(2);

#[derive(Clone, Debug, Serialize, Deserialize, PartialEq)]
pub struct Line {
    /// 0 for unsynced lyrics
    pub ms: i64,
    /// empty = an instrumental gap, nothing to highlight
    pub text: String,
}

#[derive(Clone, Debug, Serialize, Deserialize, Default, PartialEq)]
pub struct Lyrics {
    pub synced: bool,
    pub lines: Vec<Line>,
}

/// What is known about the playing track.
#[derive(Clone, Debug)]
pub struct Track {
    pub title: String,
    pub artists: String, // joined with ", "
    pub album: String,
    pub duration_sec: i64,
}

// ---- parsing ----

fn trim(s: &str) -> &str {
    s.trim_matches(|c| c == ' ' || c == '\t' || c == '\r' || c == '\n')
}

/// LRCLIB text sometimes carries invisible direction/zero-width marks the font has no glyph for: drop them.
fn strip_invisible(s: &str) -> String {
    s.chars()
        .filter(|c| !matches!(*c as u32, 0x200B..=0x200F | 0x202A..=0x202E | 0x2060 | 0xFEFF))
        .collect()
}

/// `[mm:ss]` or `[mm:ss.xx]` at the start of `s`: (milliseconds, bytes used). Tags like `[ar:Artist]` do not match.
fn parse_stamp(s: &str) -> Option<(i64, usize)> {
    let close = s.find(']')?;
    let inner = s.strip_prefix('[')?.get(..close - 1)?;
    let (mm, rest) = inner.split_once(':')?;
    let (ss, frac) = match rest.split_once('.') {
        Some((a, b)) => (a, Some(b)),
        None => (rest, None),
    };
    let digits = |x: &str| !x.is_empty() && x.bytes().all(|b| b.is_ascii_digit());
    if !digits(mm) || !digits(ss) || mm.len() > 9 || ss.len() > 9 {
        return None;
    }
    let ms_frac = match frac {
        None => 0,
        Some(f) if !f.is_empty() && f.bytes().all(|b| b.is_ascii_digit()) => {
            let mut f3: String = f.chars().take(3).collect();
            while f3.len() < 3 {
                f3.push('0');
            }
            f3.parse::<i64>().ok()?
        }
        _ => return None,
    };
    Some((
        (mm.parse::<i64>().ok()? * 60 + ss.parse::<i64>().ok()?) * 1000 + ms_frac,
        close + 1,
    ))
}

pub fn parse_lrc(lrc: &str) -> Lyrics {
    let mut lines = Vec::new();
    for line in lrc.split('\n') {
        let mut stamps = Vec::new();
        let mut i = 0;
        while line[i..].starts_with('[') {
            let Some(close) = line[i..].find(']') else {
                break;
            };
            if let Some((ms, _)) = parse_stamp(&line[i..]) {
                stamps.push(ms);
            }
            i += close + 1;
        }
        if stamps.is_empty() {
            continue;
        }
        let text = trim(&strip_invisible(&line[i..])).to_string();
        for ms in stamps {
            lines.push(Line {
                ms,
                text: text.clone(),
            });
        }
    }
    lines.sort_by_key(|l| l.ms); // stable
    Lyrics {
        synced: true,
        lines,
    }
}

pub fn parse_plain(text: &str) -> Lyrics {
    let lines = text
        .split('\n')
        .map(|l| trim(&strip_invisible(l)).to_string())
        .filter(|l| !l.is_empty())
        .map(|text| Line { ms: 0, text })
        .collect();
    Lyrics {
        synced: false,
        lines,
    }
}

/// Synced lyrics rarely say when the last line ends, so it would stay highlighted through a long outro. Append an empty
/// "gap" after it, a typical line length later, so the highlight clears once the singing stops.
fn add_end_marker(l: &mut Lyrics) {
    match l.lines.last() {
        None => return,
        Some(last) if last.text.is_empty() => return, // the LRC already ends with a gap
        _ => {}
    }
    let mut gaps: Vec<i64> = Vec::new();
    let mut prev: i64 = -1;
    for line in &l.lines {
        if line.text.is_empty() {
            prev = -1;
            continue;
        }
        if prev >= 0 && line.ms > prev {
            gaps.push(line.ms - prev);
        }
        prev = line.ms;
    }
    let mut len = 5000;
    if !gaps.is_empty() {
        gaps.sort();
        len = gaps[gaps.len() / 2];
    }
    let len = len.clamp(3000, 8000);
    let at = l.lines.last().map(|x| x.ms).unwrap_or(0) + len;
    l.lines.push(Line {
        ms: at,
        text: String::new(),
    });
}

fn jstr<'a>(j: &'a Value, key: &str) -> &'a str {
    j.get(key).and_then(Value::as_str).unwrap_or("")
}

fn from_json(j: &Value, allow_synced: bool) -> Option<Lyrics> {
    if !j.is_object() {
        return None;
    }
    let synced = if allow_synced {
        jstr(j, "syncedLyrics")
    } else {
        ""
    };
    if !synced.is_empty() {
        let mut l = parse_lrc(synced);
        if l.lines.iter().any(|x| !x.text.is_empty()) {
            add_end_marker(&mut l);
            return Some(l);
        }
    }
    let plain = jstr(j, "plainLyrics");
    if !plain.is_empty() {
        let l = parse_plain(plain);
        if !l.lines.is_empty() {
            return Some(l);
        }
    }
    None
}

// ---- ranking ----

/// Title without a trailing "(feat. ...)" / "[Remastered]" style suffix.
fn strip_suffix(t: &str) -> String {
    let mut t = t.to_string();
    for open in [" (", " ["] {
        if let Some(p) = t.rfind(open)
            && p > 0
        {
            t.truncate(p);
        }
    }
    t
}

fn first_artist(artists: &str) -> String {
    trim(artists.split(',').next().unwrap_or("")).to_string()
}

/// Share of kana/Hangul/Han among all Latin letters + those characters: entries that merely add a translation line to English
/// lyrics stay low, original-script lyrics are high.
fn native_script_ratio(l: &Lyrics) -> f64 {
    let (mut native, mut latin) = (0usize, 0usize);
    for line in &l.lines {
        for c in line.text.chars() {
            let cp = c as u32;
            if c.is_ascii_alphabetic() {
                latin += 1;
            } else if (0x3040..=0x30FF).contains(&cp)
                || (0xAC00..=0xD7A3).contains(&cp)
                || (0x4E00..=0x9FFF).contains(&cp)
            {
                native += 1;
            }
        }
    }
    if native + latin == 0 {
        0.0
    } else {
        native as f64 / (native + latin) as f64
    }
}

/// Variants LRCLIB files next to the studio version: skipped unless the playing title has the same tag.
fn has_variant_tag(title: &str) -> bool {
    let t = title.to_lowercase();
    [
        "remix",
        "live",
        "karaoke",
        "instrumental",
        "cover",
        "acoustic",
        "sped up",
        "slowed",
    ]
    .iter()
    .any(|tag| t.contains(tag))
}

/// Timestamps running past the end of the track belong to a different edit (e.g. a 10 min remix).
fn synced_fits(item: &Value, t: &Track) -> bool {
    let synced = jstr(item, "syncedLyrics");
    if synced.is_empty() {
        return false;
    }
    if t.duration_sec <= 0 {
        return true;
    }
    let l = parse_lrc(synced);
    l.lines
        .last()
        .is_some_and(|last| last.ms / 1000 <= t.duration_sec + 10)
}

/// Higher is better; negative means "do not use". `exact` marks the /get result.
fn score(item: &Value, rank: usize, exact: bool, t: &Track) -> i64 {
    let synced = jstr(item, "syncedLyrics");
    let plain = jstr(item, "plainLyrics");
    if synced.is_empty() && plain.is_empty() {
        return -1;
    }
    if item.get("instrumental").and_then(Value::as_bool) == Some(true) {
        return -1;
    }
    let mut score: i64 = 0;
    if t.duration_sec > 0
        && let Some(d) = item.get("duration").and_then(Value::as_f64)
    {
        let diff = (d - t.duration_sec as f64).abs();
        if diff > 5.0 {
            return -1;
        }
        score += 100 - (diff * 10.0) as i64; // closer length = same recording
    }
    let use_synced = synced_fits(item, t);
    if !use_synced && plain.is_empty() {
        return -1;
    }
    if use_synced {
        score += 100;
    }
    if has_variant_tag(jstr(item, "trackName")) && !has_variant_tag(&t.title) {
        score -= 80;
    }
    // LRCLIB holds romanized and original-script versions of one song: prefer the original only when the text is mostly in that
    // script (so English lyrics + a translation line do not win)
    let text = if use_synced {
        parse_lrc(synced)
    } else {
        parse_plain(plain)
    };
    if native_script_ratio(&text) >= 0.5 {
        score += 60;
    }
    if !t.album.is_empty() && jstr(item, "albumName") == t.album {
        score += 20;
    }
    if exact {
        score += 30;
    }
    score - rank.min(20) as i64 // tie-break by LRCLIB's own ranking
}

// ---- lookup ----

fn client() -> &'static reqwest::Client {
    static C: OnceLock<reqwest::Client> = OnceLock::new();
    C.get_or_init(|| {
        reqwest::Client::builder()
            .user_agent("raira/0.1 (https://github.com/nanowater/Raira)")
            .connect_timeout(Duration::from_secs(4))
            .timeout(Duration::from_secs(10))
            .build()
            .expect("http client")
    })
}

enum Answer {
    Body(String),
    NotFound, // a definite "no": HTTP 404
    Failed,   // no usable answer (network, 5xx, rate limit that did not clear)
}

/// LRCLIB answers 429 when requests come in bursts: back off and retry a couple of times.
async fn get(url: &str) -> Answer {
    for attempt in 0..3u64 {
        match client().get(url).send().await {
            Ok(resp) if resp.status().is_success() => {
                return resp
                    .text()
                    .await
                    .map(Answer::Body)
                    .unwrap_or(Answer::Failed);
            }
            Ok(resp) if resp.status().as_u16() == 404 => return Answer::NotFound,
            Ok(resp) if resp.status().as_u16() == 429 => {
                tracing::warn!(
                    "lyrics: LRCLIB answered 429 (attempt {}) for {url}",
                    attempt + 1
                );
                tokio::time::sleep(Duration::from_millis(1500 * (attempt + 1))).await
            }
            Ok(resp) => {
                tracing::warn!("lyrics: LRCLIB answered {} for {url}", resp.status());
                return Answer::Failed;
            }
            Err(e) => {
                tracing::warn!("lyrics: request failed for {url}: {e}");
                return Answer::Failed;
            }
        }
    }
    tracing::warn!("lyrics: still 429 after the retries for {url}");
    Answer::Failed
}

fn esc(s: &str) -> String {
    reqwest::Url::parse_with_params("http://x/", &[("q", s)])
        .map(|u| u.query().unwrap_or("").trim_start_matches("q=").to_string())
        .unwrap_or_default()
}

/// The ranking part, separate from the network so it can be tested: the best candidate's lyrics, if any is usable.
fn pick(cands: &[(Value, bool)], t: &Track) -> Option<Lyrics> {
    let mut best: Option<(&Value, i64)> = None;
    for (rank, (item, exact)) in cands.iter().enumerate() {
        let sc = score(item, rank, *exact, t);
        if sc > best.map(|b| b.1).unwrap_or(-1) {
            best = Some((item, sc));
        }
    }
    let item = best?.0;
    from_json(item, synced_fits(item, t))
}

/// Ok(None) = LRCLIB has nothing usable for this track; Err = could not find out (do not remember that as "none").
pub async fn lookup_network(t: &Track) -> Result<Option<Lyrics>, ()> {
    if t.title.is_empty() {
        return Ok(None);
    }
    // Candidates: the exact lookup (title/artist/album/duration must match closely) plus a free-text search like the LRCLIB
    // website, which also finds entries filed under another title form ("あぶく - Bubble" for "Bubble"). We rank them ourselves.
    let mut cands: Vec<(Value, bool)> = Vec::new();
    let mut answered = false;
    let mut failed = false; // a request that gave no usable answer

    let base = base();
    let mut url = format!(
        "{base}get?track_name={}&artist_name={}",
        esc(&t.title),
        esc(&t.artists)
    );
    if !t.album.is_empty() {
        url += &format!("&album_name={}", esc(&t.album));
    }
    if t.duration_sec > 0 {
        url += &format!("&duration={}", t.duration_sec);
    }
    match get(&url).await {
        Answer::Body(b) => {
            answered = true;
            if let Ok(v) = serde_json::from_str::<Value>(&b)
                && v.is_object()
            {
                cands.push((v, true));
            }
        }
        Answer::NotFound => answered = true,
        Answer::Failed => failed = true,
    }

    let q = format!("{} {}", strip_suffix(&t.title), first_artist(&t.artists));
    match get(&format!("{base}search?q={}", esc(&q))).await {
        Answer::Body(b) => {
            answered = true;
            if let Ok(Value::Array(items)) = serde_json::from_str::<Value>(&b) {
                cands.extend(
                    items
                        .into_iter()
                        .filter(Value::is_object)
                        .map(|i| (i, false)),
                );
            }
        }
        Answer::NotFound => answered = true,
        Answer::Failed => failed = true,
    }
    // "nothing found" is only believed when both requests were answered: with one of them lost the candidates may simply be missing
    if !answered || (failed && cands.is_empty()) {
        return Err(());
    }
    let picked = pick(&cands, t);
    tracing::info!(
        "lyrics: \"{}\" — {} candidate(s), {}",
        t.title,
        cands.len(),
        if picked.is_some() {
            "found"
        } else {
            "none usable"
        }
    );
    Ok(picked)
}

// ---- cache ----

fn cache_dir() -> PathBuf {
    let base = std::env::var_os("XDG_CACHE_HOME")
        .map(PathBuf::from)
        .or_else(|| std::env::var_os("HOME").map(|h| PathBuf::from(h).join(".cache")))
        .unwrap_or_else(|| PathBuf::from("."));
    base.join("raira-bridge").join("lyrics")
}

fn key_of(t: &Track) -> String {
    let k = format!(
        "{}|{}|{}|{}",
        t.title.to_lowercase(),
        t.artists.to_lowercase(),
        t.album.to_lowercase(),
        t.duration_sec
    );
    format!(
        "{:016x}",
        k.bytes().fold(0xcbf29ce484222325u64, |h, b| (h ^ b as u64)
            .wrapping_mul(0x100000001b3))
    )
}

#[derive(Serialize, Deserialize)]
struct Cached {
    at: u64, // unix seconds
    /// "title — artists", only to tell the entries apart when looking into the cache by hand
    #[serde(default)]
    track: String,
    lyrics: Option<Lyrics>,
}

fn now() -> u64 {
    std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.as_secs())
        .unwrap_or(0)
}

fn cache_read(t: &Track) -> Option<Option<Lyrics>> {
    let text = std::fs::read_to_string(cache_dir().join(format!("{}.json", key_of(t)))).ok()?;
    let c: Cached = serde_json::from_str(&text).ok()?;
    match &c.lyrics {
        Some(_) => Some(c.lyrics), // found: kept
        None if now().saturating_sub(c.at) < NEGATIVE_TTL.as_secs() => Some(None), // "none" is believed for a week
        None => None,
    }
}

fn cache_write(t: &Track, lyrics: &Option<Lyrics>) {
    let dir = cache_dir();
    if std::fs::create_dir_all(&dir).is_ok() {
        let c = Cached {
            at: now(),
            track: format!("{} — {}", t.title, t.artists),
            lyrics: lyrics.clone(),
        };
        let _ = std::fs::write(
            dir.join(format!("{}.json", key_of(t))),
            serde_json::to_string(&c).unwrap_or_default(),
        );
    }
}

/// One lookup at a time, and only for tracks not in the cache: LRCLIB rate-limits (HTTP 429).
pub async fn lookup(t: &Track) -> Result<Option<Lyrics>, ()> {
    static ONE_AT_A_TIME: Semaphore = Semaphore::const_new(1);
    if let Some(hit) = cache_read(t) {
        return Ok(hit);
    }
    let _permit = ONE_AT_A_TIME.acquire().await.map_err(|_| ())?;
    if let Some(hit) = cache_read(t) {
        // another request may have filled it while we waited
        return Ok(hit);
    }
    let mut found = lookup_network(t).await;
    if !matches!(found, Ok(Some(_))) {
        tokio::time::sleep(RETRY_AFTER).await; // a blip of LRCLIB rarely lasts twice
        let again = lookup_network(t).await;
        if matches!(again, Ok(Some(_))) || found.is_err() {
            found = again;
        }
    }
    let found = found?; // Err: not remembered, the next request for this track asks again
    cache_write(t, &found);
    Ok(found)
}

/// The lines as they go to the device, cut to the message size limit.
pub fn for_device(mut l: Lyrics) -> Lyrics {
    let mut size = 0usize;
    let mut keep = l.lines.len();
    for (i, line) in l.lines.iter().enumerate() {
        size += line.text.len() + 24;
        if size > MAX_LINES_BYTES {
            keep = i;
            break;
        }
    }
    l.lines.truncate(keep);
    l
}

#[cfg(test)]
mod tests {
    use super::*;

    fn t(title: &str, artists: &str, album: &str, dur: i64) -> Track {
        Track {
            title: title.into(),
            artists: artists.into(),
            album: album.into(),
            duration_sec: dur,
        }
    }

    #[test]
    fn lrc_stamps_fraction_and_tags() {
        let l = parse_lrc("[ar:Someone]\n[00:12.5]one\n[01:02.345][01:30]two\n[00:05]\nno stamp\n");
        let got: Vec<(i64, &str)> = l.lines.iter().map(|x| (x.ms, x.text.as_str())).collect();
        assert_eq!(
            got,
            vec![(5000, ""), (12500, "one"), (62345, "two"), (90000, "two")]
        );
    }

    #[test]
    fn invisible_marks_are_dropped() {
        assert_eq!(
            parse_plain("\u{200E}hello\u{200B} world\n\n  \n").lines,
            vec![Line {
                ms: 0,
                text: "hello world".into()
            }]
        );
    }

    #[test]
    fn end_marker_uses_the_median_line_gap_within_limits() {
        let mut l = parse_lrc("[00:10.00]a\n[00:14.00]b\n[00:18.00]c\n");
        add_end_marker(&mut l);
        assert_eq!(
            l.lines.last(),
            Some(&Line {
                ms: 22000,
                text: String::new()
            })
        );
        let mut one = parse_lrc("[00:10.00]a\n");
        add_end_marker(&mut one);
        assert_eq!(one.lines.last().map(|x| x.ms), Some(15000)); // no gaps known: 5 s
    }

    #[test]
    fn title_helpers() {
        assert_eq!(strip_suffix("Song (feat. X) [Remastered]"), "Song");
        assert_eq!(first_artist("A, B"), "A");
        assert!(has_variant_tag("Song (Live)") && !has_variant_tag("Song"));
    }

    #[test]
    fn ranking_prefers_original_script_synced_and_close_length() {
        let romaji = serde_json::json!({"trackName":"Bubble","albumName":"x","duration":200.0,"syncedLyrics":"[00:01.00]ima ga subete\n[00:05.00]kimi to boku"});
        let original = serde_json::json!({"trackName":"あぶく - Bubble","albumName":"x","duration":201.0,"syncedLyrics":"[00:01.00]今がすべて\n[00:05.00]君と僕"});
        let karaoke = serde_json::json!({"trackName":"Bubble (Karaoke)","albumName":"x","duration":200.0,"syncedLyrics":"[00:01.00]今がすべて"});
        let tr = t("Bubble", "Eve", "x", 200);
        let got = pick(&[(romaji, true), (karaoke, false), (original, false)], &tr).unwrap();
        assert_eq!(got.lines[0].text, "今がすべて");
        // a different edit (its timestamps run far past the track) is not used as synced
        let long = serde_json::json!({"trackName":"Bubble","duration":200.0,"syncedLyrics":"[09:00.00]late","plainLyrics":"plain text"});
        let got = pick(&[(long, true)], &tr).unwrap();
        assert!(!got.synced && got.lines[0].text == "plain text");
        // too different in length
        let off = serde_json::json!({"trackName":"Bubble","duration":260.0,"plainLyrics":"x"});
        assert!(pick(&[(off, true)], &tr).is_none());
    }

    #[test]
    fn device_cut_keeps_messages_small() {
        let l = Lyrics {
            synced: true,
            lines: (0..5000)
                .map(|i| Line {
                    ms: i,
                    text: "x".repeat(40),
                })
                .collect(),
        };
        let cut = for_device(l);
        assert!(cut.lines.len() < 5000 && cut.lines.len() > 100);
    }
}
