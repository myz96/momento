use std::fs;
use std::path::{Path, PathBuf};
use std::time::{Duration, SystemTime, UNIX_EPOCH};

use serde::{Deserialize, Serialize};
use tauri::ipc::Channel;
use tauri::{AppHandle, Manager};

const DEFAULT_BASE: &str = "http://192.168.4.1";

#[derive(Serialize, Deserialize)]
struct DeviceInfo {
    device: String,
    files: u32,
    total_bytes: u64,
}

#[derive(Deserialize)]
struct DeviceFile {
    name: String,
    size: u64,
}

#[derive(Serialize, Clone)]
struct SyncProgress {
    file: String,
    index: u32,
    total: u32,
}

#[derive(Serialize)]
struct LocalFile {
    name: String,
    path: String,
    size: u64,
    kind: String,
    modified_ms: u64,
}

fn err_str<E: std::fmt::Display>(e: E) -> String {
    e.to_string()
}

fn http_client() -> Result<reqwest::Client, String> {
    reqwest::Client::builder()
        .connect_timeout(Duration::from_secs(5))
        .timeout(Duration::from_secs(120))
        .build()
        .map_err(err_str)
}

fn base_url(base: Option<String>) -> String {
    let b = base.unwrap_or_default();
    if b.trim().is_empty() {
        DEFAULT_BASE.to_string()
    } else {
        b.trim().trim_end_matches('/').to_string()
    }
}

fn media_dir(app: &AppHandle) -> Result<PathBuf, String> {
    let dir = app.path().app_data_dir().map_err(err_str)?.join("media");
    fs::create_dir_all(&dir).map_err(err_str)?;
    Ok(dir)
}

fn kind_of(name: &str) -> String {
    let lower = name.to_lowercase();
    if lower.ends_with(".jpg") || lower.ends_with(".jpeg") {
        "photo"
    } else if lower.ends_with(".wav") {
        "audio"
    } else if lower.ends_with(".avi") {
        "video"
    } else {
        "other"
    }
    .to_string()
}

fn local_file_meta(path: &Path) -> Result<LocalFile, String> {
    let meta = fs::metadata(path).map_err(err_str)?;
    let modified_ms = meta
        .modified()
        .ok()
        .and_then(|t| t.duration_since(UNIX_EPOCH).ok())
        .map(|d| d.as_millis() as u64)
        .unwrap_or(0);
    let name = path
        .file_name()
        .map(|n| n.to_string_lossy().to_string())
        .unwrap_or_default();
    Ok(LocalFile {
        kind: kind_of(&name),
        name,
        path: path.to_string_lossy().to_string(),
        size: meta.len(),
        modified_ms,
    })
}

#[tauri::command]
async fn device_info(base: Option<String>) -> Result<DeviceInfo, String> {
    let client = http_client()?;
    client
        .get(format!("{}/api/info", base_url(base)))
        .send()
        .await
        .map_err(|e| format!("Device not reachable: {e}"))?
        .error_for_status()
        .map_err(err_str)?
        .json()
        .await
        .map_err(err_str)
}

/// Downloads every file from the device, verifies the size, stores it in
/// the app media dir, then deletes it from the device. A file only leaves
/// the SD card after the local copy is verified.
#[tauri::command]
async fn sync_device(
    app: AppHandle,
    base: Option<String>,
    on_progress: Channel<SyncProgress>,
) -> Result<Vec<LocalFile>, String> {
    let base = base_url(base);
    let client = http_client()?;

    let files: Vec<DeviceFile> = client
        .get(format!("{base}/api/files"))
        .send()
        .await
        .map_err(|e| format!("Device not reachable: {e}"))?
        .error_for_status()
        .map_err(err_str)?
        .json()
        .await
        .map_err(err_str)?;

    let dir = media_dir(&app)?;
    let total = files.len() as u32;
    let mut synced = Vec::new();

    for (i, f) in files.iter().enumerate() {
        let _ = on_progress.send(SyncProgress {
            file: f.name.clone(),
            index: i as u32 + 1,
            total,
        });

        let bytes = client
            .get(format!("{base}/api/files/{}", f.name))
            .send()
            .await
            .map_err(err_str)?
            .error_for_status()
            .map_err(err_str)?
            .bytes()
            .await
            .map_err(err_str)?;

        if bytes.len() as u64 != f.size {
            return Err(format!(
                "Size mismatch for {}: got {} of {} bytes. The file stays on the device.",
                f.name,
                bytes.len(),
                f.size
            ));
        }

        // A timestamp prefix keeps names unique: device indexes restart
        // at 001 after every sync.
        let stamp = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .map_err(err_str)?
            .as_millis();
        let path = dir.join(format!("{stamp}_{}", f.name));
        fs::write(&path, &bytes).map_err(err_str)?;

        client
            .delete(format!("{base}/api/files/{}", f.name))
            .send()
            .await
            .map_err(err_str)?
            .error_for_status()
            .map_err(|e| format!("Saved {} but the device delete failed: {e}", f.name))?;

        synced.push(local_file_meta(&path)?);
    }

    Ok(synced)
}

#[tauri::command]
fn list_local_media(app: AppHandle) -> Result<Vec<LocalFile>, String> {
    let dir = media_dir(&app)?;
    let mut out = Vec::new();
    for entry in fs::read_dir(&dir).map_err(err_str)? {
        let entry = entry.map_err(err_str)?;
        let path = entry.path();
        if path.is_file() && kind_of(&path.to_string_lossy()) != "other" {
            out.push(local_file_meta(&path)?);
        }
    }
    out.sort_by(|a, b| b.modified_ms.cmp(&a.modified_ms));
    Ok(out)
}

#[tauri::command]
fn open_media(path: String) -> Result<(), String> {
    tauri_plugin_opener::open_path(path, None::<String>).map_err(err_str)
}

#[cfg_attr(mobile, tauri::mobile_entry_point)]
pub fn run() {
    tauri::Builder::default()
        .plugin(tauri_plugin_opener::init())
        .invoke_handler(tauri::generate_handler![
            device_info,
            sync_device,
            list_local_media,
            open_media
        ])
        .run(tauri::generate_context!())
        .expect("error while running tauri application");
}
