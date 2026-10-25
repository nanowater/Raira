//! Cover art: fetch from wherever the player keeps it (file or URL), shrink, re-encode as baseline JPEG
//! (the device's decoder never has to deal with progressive JPEGs, PNG or WebP).

use std::sync::OnceLock;

use image::codecs::jpeg::JpegEncoder;

fn client() -> &'static reqwest::Client {
    static C: OnceLock<reqwest::Client> = OnceLock::new();
    C.get_or_init(|| {
        reqwest::Client::builder()
            .timeout(std::time::Duration::from_secs(10))
            .build()
            .expect("http client")
    })
}

fn percent_decode(s: &str) -> Vec<u8> {
    let b = s.as_bytes();
    let mut out = Vec::with_capacity(b.len());
    let mut i = 0;
    while i < b.len() {
        if b[i] == b'%' && i + 2 < b.len() {
            let hex = std::str::from_utf8(&b[i + 1..i + 3])
                .ok()
                .and_then(|h| u8::from_str_radix(h, 16).ok());
            if let Some(v) = hex {
                out.push(v);
                i += 3;
                continue;
            }
        }
        out.push(b[i]);
        i += 1;
    }
    out
}

pub async fn load(url: &str) -> Result<Vec<u8>, String> {
    if let Some(path) = url.strip_prefix("file://") {
        let path = String::from_utf8_lossy(&percent_decode(path)).into_owned();
        return tokio::fs::read(&path)
            .await
            .map_err(|e| format!("{path}: {e}"));
    }
    if url.starts_with("http://") || url.starts_with("https://") {
        let resp = client().get(url).send().await.map_err(|e| e.to_string())?;
        if !resp.status().is_success() {
            return Err(format!("HTTP {}", resp.status()));
        }
        return resp
            .bytes()
            .await
            .map(|b| b.to_vec())
            .map_err(|e| e.to_string());
    }
    Err(format!("unsupported cover url scheme: {url}"))
}

pub async fn to_jpeg(bytes: Vec<u8>, px: u32) -> Result<Vec<u8>, String> {
    tokio::task::spawn_blocking(move || {
        let img = image::load_from_memory(&bytes).map_err(|e| e.to_string())?;
        let rgb = img.thumbnail(px, px).to_rgb8();
        let mut out = Vec::new();
        JpegEncoder::new_with_quality(&mut out, 85)
            .encode_image(&rgb)
            .map_err(|e| e.to_string())?;
        Ok(out)
    })
    .await
    .map_err(|e| e.to_string())?
}
