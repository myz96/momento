//! Cloud offload: thumbnails and the local index of offloaded files.
//!
//! After a verified upload, the full local file is replaced by a small
//! thumbnail plus an entry in offloaded.json. The gallery lists these
//! entries as `location: "cloud"` and streams the full file from the
//! backend when needed.

use std::fs;
use std::path::{Path, PathBuf};

use serde::{Deserialize, Serialize};

const THUMB_MAX_PX: u32 = 480;

#[derive(Serialize, Deserialize, Clone)]
pub struct OffloadedFile {
    pub name: String,
    pub size: u64,
    pub kind: String,
    pub modified_ms: u64,
    pub thumb: Option<String>,
}

fn index_path(data_dir: &Path) -> PathBuf {
    data_dir.join("offloaded.json")
}

pub fn thumbs_dir(data_dir: &Path) -> Result<PathBuf, String> {
    let dir = data_dir.join("thumbs");
    fs::create_dir_all(&dir).map_err(|e| e.to_string())?;
    Ok(dir)
}

pub fn load_index(data_dir: &Path) -> Vec<OffloadedFile> {
    fs::read(index_path(data_dir))
        .ok()
        .and_then(|bytes| serde_json::from_slice(&bytes).ok())
        .unwrap_or_default()
}

pub fn save_index(data_dir: &Path, index: &[OffloadedFile]) -> Result<(), String> {
    let json = serde_json::to_vec_pretty(index).map_err(|e| e.to_string())?;
    fs::write(index_path(data_dir), json).map_err(|e| e.to_string())
}

/// Finds the first JPEG frame inside an MJPEG AVI. The device's clips are
/// concatenated JPEGs, so a byte scan for the SOI/EOI markers is enough.
pub fn extract_mjpeg_frame(bytes: &[u8]) -> Option<Vec<u8>> {
    let start = bytes.windows(3).position(|w| w == [0xFF, 0xD8, 0xFF])?;
    let rest = &bytes[start..];
    let end = rest.windows(2).position(|w| w == [0xFF, 0xD9])? + 2;
    Some(rest[..end].to_vec())
}

/// Writes a JPEG thumbnail for a photo or clip. Returns Ok(None) for
/// audio and other kinds that have no visual to shrink.
pub fn make_thumbnail(
    src: &Path,
    kind: &str,
    thumbs: &Path,
) -> Result<Option<PathBuf>, String> {
    let jpeg_bytes = match kind {
        "photo" => fs::read(src).map_err(|e| e.to_string())?,
        "video" => {
            let bytes = fs::read(src).map_err(|e| e.to_string())?;
            match extract_mjpeg_frame(&bytes) {
                Some(frame) => frame,
                None => return Ok(None),
            }
        }
        _ => return Ok(None),
    };

    let img = image::load_from_memory(&jpeg_bytes)
        .map_err(|e| format!("Thumbnail decode failed: {e}"))?;
    let thumb = img.thumbnail(THUMB_MAX_PX, THUMB_MAX_PX);

    let name = src
        .file_name()
        .map(|n| n.to_string_lossy().to_string())
        .unwrap_or_default();
    let dst = thumbs.join(format!("{name}.thumb.jpg"));
    thumb
        .to_rgb8()
        .save_with_format(&dst, image::ImageFormat::Jpeg)
        .map_err(|e| format!("Thumbnail save failed: {e}"))?;
    Ok(Some(dst))
}

#[cfg(test)]
mod tests {
    use super::*;

    fn tiny_jpeg() -> Vec<u8> {
        let img = image::RgbImage::from_pixel(64, 48, image::Rgb([120, 40, 200]));
        let mut out = std::io::Cursor::new(Vec::new());
        image::DynamicImage::ImageRgb8(img)
            .write_to(&mut out, image::ImageFormat::Jpeg)
            .unwrap();
        out.into_inner()
    }

    #[test]
    fn extracts_first_frame_from_avi_bytes() {
        let frame = tiny_jpeg();
        let mut avi = b"RIFFxxxxAVI LISTmovi00dc".to_vec();
        avi.extend_from_slice(&frame);
        avi.extend_from_slice(b"00dc");
        avi.extend_from_slice(&frame);
        let got = extract_mjpeg_frame(&avi).expect("frame found");
        assert!(got.starts_with(&[0xFF, 0xD8]));
        assert!(got.ends_with(&[0xFF, 0xD9]));
        image::load_from_memory(&got).expect("frame decodes");
    }

    #[test]
    fn photo_thumbnail_roundtrip() {
        let dir = std::env::temp_dir().join("momento-thumb-test");
        fs::create_dir_all(&dir).unwrap();
        let src = dir.join("PHOTO_TEST.JPG");
        fs::write(&src, tiny_jpeg()).unwrap();
        let thumb = make_thumbnail(&src, "photo", &dir).unwrap().unwrap();
        image::open(&thumb).expect("thumbnail decodes");
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn audio_has_no_thumbnail() {
        let dir = std::env::temp_dir().join("momento-thumb-test-audio");
        fs::create_dir_all(&dir).unwrap();
        let src = dir.join("AUD_001.WAV");
        fs::write(&src, b"RIFF....WAVE").unwrap();
        assert!(make_thumbnail(&src, "audio", &dir).unwrap().is_none());
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn index_roundtrip() {
        let dir = std::env::temp_dir().join("momento-index-test");
        fs::create_dir_all(&dir).unwrap();
        let entries = vec![OffloadedFile {
            name: "X.JPG".into(),
            size: 10,
            kind: "photo".into(),
            modified_ms: 1234,
            thumb: Some("thumbs/X.JPG.thumb.jpg".into()),
        }];
        save_index(&dir, &entries).unwrap();
        let loaded = load_index(&dir);
        assert_eq!(loaded.len(), 1);
        assert_eq!(loaded[0].name, "X.JPG");
        let _ = fs::remove_dir_all(&dir);
    }
}
