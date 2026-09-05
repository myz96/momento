mod offload;

use std::fs;
use std::path::{Path, PathBuf};
use std::time::{Duration, SystemTime, UNIX_EPOCH};

use offload::OffloadedFile;

use btleplug::api::{
    Central, Manager as BleManager, Peripheral as _, ScanFilter, WriteType,
};
use btleplug::platform::{Manager as PlatformBleManager, Peripheral};
use serde::{Deserialize, Serialize};
use tauri::ipc::Channel;
use tauri::{AppHandle, Manager};
use uuid::Uuid;

const DEFAULT_BASE: &str = "http://192.168.4.1";
const BLE_DEVICE_NAME: &str = "Momento";

/// One store operation at a time: a backup snapshotting the media dir
/// while a sync writes into it could upload a partial file, "verify"
/// it, and then delete the completed original.
static STORE_LOCK: tokio::sync::Mutex<()> = tokio::sync::Mutex::const_new(());

const CHR_SSID: Uuid = Uuid::from_u128(0x6d6f6d65_6e74_6f00_0000_000000000002);
const CHR_PASS: Uuid = Uuid::from_u128(0x6d6f6d65_6e74_6f00_0000_000000000003);
const CHR_CONTROL: Uuid = Uuid::from_u128(0x6d6f6d65_6e74_6f00_0000_000000000004);
const CHR_STATUS: Uuid = Uuid::from_u128(0x6d6f6d65_6e74_6f00_0000_000000000005);

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
    /// Capture time in unix seconds. Only real once the device clock is
    /// set (SNTP in sync mode); bogus values stay near the 1980 epoch.
    #[serde(default)]
    mtime: i64,
}

const CREDIBLE_MTIME_FLOOR: i64 = 1_609_459_200; // 2021-01-01

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
    /// "local" = full file on disk, "cloud" = offloaded, thumbnail only.
    location: String,
    thumb: Option<String>,
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

fn data_dir(app: &AppHandle) -> Result<PathBuf, String> {
    let dir = app.path().app_data_dir().map_err(err_str)?;
    fs::create_dir_all(&dir).map_err(err_str)?;
    Ok(dir)
}

fn media_dir(app: &AppHandle) -> Result<PathBuf, String> {
    let dir = data_dir(app)?.join("media");
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
        location: "local".to_string(),
        thumb: None,
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
    let _guard = STORE_LOCK.lock().await;
    let base = base_url(base);
    let client = http_client()?;

    // Reap stale .part files from earlier crashes; they are invisible to
    // listings but waste disk.
    if let Ok(dir) = media_dir(&app) {
        if let Ok(entries) = fs::read_dir(&dir) {
            for entry in entries.flatten() {
                let p = entry.path();
                if p.extension().is_some_and(|e| e == "part") {
                    let _ = fs::remove_file(&p);
                }
            }
        }
    }

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
        // Write to a .part name, force to disk, then rename: listings
        // never see a partial file, and the device copy is only deleted
        // after the local bytes are durable.
        let path = dir.join(format!("{stamp}_{}", f.name));
        let part = dir.join(format!("{stamp}_{}.part", f.name));
        let write_result = (|| -> std::io::Result<()> {
            use std::io::Write;
            let mut out = fs::File::create(&part)?;
            out.write_all(&bytes)?;
            out.sync_all()
        })();
        if let Err(e) = write_result {
            let _ = fs::remove_file(&part);
            return Err(format!("Saving {} failed: {e}", f.name));
        }
        fs::rename(&part, &path).map_err(err_str)?;
        if f.mtime > CREDIBLE_MTIME_FLOOR {
            let t = filetime::FileTime::from_unix_time(f.mtime, 0);
            let _ = filetime::set_file_mtime(&path, t);
        }

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
            // A file can vanish between read_dir and stat when a backup
            // offloads it concurrently; skip it rather than fail the
            // whole listing.
            if let Ok(meta) = local_file_meta(&path) {
                out.push(meta);
            }
        }
    }
    // A cloud entry whose full file still exists locally is a crash
    // leftover; show the local copy only.
    let local_names: std::collections::HashSet<String> =
        out.iter().map(|f| f.name.clone()).collect();
    for f in offload::load_index(&data_dir(&app)?) {
        if local_names.contains(&f.name) {
            continue;
        }
        out.push(LocalFile {
            name: f.name,
            path: String::new(),
            size: f.size,
            kind: f.kind,
            modified_ms: f.modified_ms,
            location: "cloud".to_string(),
            thumb: f.thumb,
        });
    }
    out.sort_by(|a, b| b.modified_ms.cmp(&a.modified_ms));
    Ok(out)
}

#[tauri::command]
fn open_media(path: String) -> Result<(), String> {
    if path.starts_with("http://") || path.starts_with("https://") {
        tauri_plugin_opener::open_url(path, None::<String>).map_err(err_str)
    } else {
        tauri_plugin_opener::open_path(path, None::<String>).map_err(err_str)
    }
}

#[derive(Serialize, Deserialize)]
struct WifiStatus {
    state: String,
    ip: String,
    ssid: String,
}

#[derive(Deserialize)]
struct CloudFile {
    name: String,
    size: u64,
}

#[derive(Serialize)]
struct BackupReport {
    uploaded: u32,
    already_backed_up: u32,
    freed: u32,
    freed_bytes: u64,
}

/// Uploads every local media file the backend does not hold yet. With
/// free_space, a verified upload is then replaced locally by a thumbnail.
#[tauri::command]
async fn backup_to_cloud(
    app: AppHandle,
    backend: Option<String>,
    free_space: bool,
    on_progress: Channel<SyncProgress>,
) -> Result<BackupReport, String> {
    let _guard = STORE_LOCK.lock().await;
    let backend = {
        let b = backend.unwrap_or_default();
        let b = b.trim();
        if b.is_empty() {
            "http://localhost:8000".to_string()
        } else {
            b.trim_end_matches('/').to_string()
        }
    };
    let client = http_client()?;

    let existing: Vec<CloudFile> = client
        .get(format!("{backend}/media"))
        .send()
        .await
        .map_err(|e| format!("Backend not reachable: {e}"))?
        .error_for_status()
        .map_err(err_str)?
        .json()
        .await
        .map_err(err_str)?;
    let cloud_sizes: std::collections::HashMap<String, u64> =
        existing.into_iter().map(|f| (f.name, f.size)).collect();

    let local = list_local_media(app.clone())?;
    let local: Vec<&LocalFile> =
        local.iter().filter(|f| f.location == "local").collect();
    // A name whose cloud size differs is a truncated earlier upload:
    // upload it again (the backend overwrite heals the copy).
    let pending: Vec<&&LocalFile> = local
        .iter()
        .filter(|f| cloud_sizes.get(&f.name) != Some(&f.size))
        .collect();

    let total = pending.len() as u32;
    let mut uploaded = 0u32;
    for (i, f) in pending.iter().enumerate() {
        let _ = on_progress.send(SyncProgress {
            file: f.name.clone(),
            index: i as u32 + 1,
            total,
        });
        let bytes = fs::read(&f.path).map_err(err_str)?;
        let part = reqwest::multipart::Part::bytes(bytes).file_name(f.name.clone());
        let form = reqwest::multipart::Form::new().part("file", part);
        client
            .post(format!("{backend}/media"))
            .multipart(form)
            .send()
            .await
            .map_err(err_str)?
            .error_for_status()
            .map_err(|e| format!("Upload of {} failed: {e}", f.name))?;
        uploaded += 1;
    }

    let mut freed = 0u32;
    let mut freed_bytes = 0u64;
    if free_space {
        // Re-read the cloud list so every local file is verified against
        // the byte count the backend actually stored.
        let verified: Vec<CloudFile> = client
            .get(format!("{backend}/media"))
            .send()
            .await
            .map_err(err_str)?
            .error_for_status()
            .map_err(err_str)?
            .json()
            .await
            .map_err(err_str)?;
        let verified: std::collections::HashMap<String, u64> =
            verified.into_iter().map(|f| (f.name, f.size)).collect();

        let root = data_dir(&app)?;
        let thumbs = offload::thumbs_dir(&root)?;
        let mut index = offload::load_index(&root);
        let mut delete_failures = 0u32;
        for f in &local {
            if verified.get(&f.name) != Some(&f.size) {
                continue; // not in the cloud, or the size differs: keep it
            }
            let src = PathBuf::from(&f.path);
            // A name already in the index is a crash leftover whose
            // delete did not land: skip the bookkeeping, retry the
            // delete. Otherwise persist the entry before the file goes,
            // so a crash leaves a duplicate, never a vanished file.
            if !index.iter().any(|e| e.name == f.name) {
                let thumb = match offload::make_thumbnail(&src, &f.kind, &thumbs)
                {
                    Ok(t) => t,
                    Err(_) => continue, // keep the full file when no thumbnail
                };
                index.push(OffloadedFile {
                    name: f.name.clone(),
                    size: f.size,
                    kind: f.kind.clone(),
                    modified_ms: f.modified_ms,
                    thumb: thumb.map(|p| p.to_string_lossy().to_string()),
                });
                offload::save_index(&root, &index)?;
            }
            if fs::remove_file(&src).is_err() {
                delete_failures += 1;
                continue; // still local; the listing hides the cloud twin
            }
            freed += 1;
            freed_bytes += f.size;
        }
        if delete_failures > 0 {
            return Err(format!(
                "{delete_failures} file(s) could not be removed locally \
                 (in use by another app?). They stay safe and the next \
                 backup retries."
            ));
        }
    }

    Ok(BackupReport {
        uploaded,
        already_backed_up: local.len() as u32 - total,
        freed,
        freed_bytes,
    })
}

async fn ble_find_device() -> Result<Peripheral, String> {
    let manager = PlatformBleManager::new().await.map_err(err_str)?;
    let adapters = manager.adapters().await.map_err(err_str)?;
    let adapter = adapters
        .into_iter()
        .next()
        .ok_or("No Bluetooth adapter found")?;

    adapter
        .start_scan(ScanFilter::default())
        .await
        .map_err(|e| format!("Bluetooth scan failed: {e}"))?;

    let deadline = SystemTime::now() + Duration::from_secs(15);
    let found = loop {
        let mut hit = None;
        for p in adapter.peripherals().await.map_err(err_str)? {
            let name = p
                .properties()
                .await
                .ok()
                .flatten()
                .and_then(|props| props.local_name);
            if name.as_deref() == Some(BLE_DEVICE_NAME) {
                hit = Some(p);
                break;
            }
        }
        if hit.is_some() {
            break hit;
        }
        if SystemTime::now() > deadline {
            break None;
        }
        tokio::time::sleep(Duration::from_millis(500)).await;
    };
    let _ = adapter.stop_scan().await;

    found.ok_or_else(|| {
        "Device not found over Bluetooth. Hold CAM for 1.5 s so the LED blinks, \
         then try again."
            .to_string()
    })
}

fn find_char(
    device: &Peripheral,
    uuid: Uuid,
) -> Result<btleplug::api::Characteristic, String> {
    device
        .characteristics()
        .into_iter()
        .find(|c| c.uuid == uuid)
        .ok_or_else(|| format!("Characteristic {uuid} not found on the device"))
}

/// Sends the home Wi-Fi credentials to the device over BLE, then polls the
/// status characteristic until the device reports a final state.
#[tauri::command]
async fn provision_wifi(ssid: String, password: String) -> Result<WifiStatus, String> {
    if ssid.trim().is_empty() {
        return Err("The network name is empty.".to_string());
    }

    let device = ble_find_device().await?;
    device
        .connect()
        .await
        .map_err(|e| format!("Bluetooth connect failed: {e}"))?;
    let result = provision_over_connection(&device, &ssid, &password).await;
    let _ = device.disconnect().await;
    result
}

async fn provision_over_connection(
    device: &Peripheral,
    ssid: &str,
    password: &str,
) -> Result<WifiStatus, String> {
    device.discover_services().await.map_err(err_str)?;

    let chr_ssid = find_char(device, CHR_SSID)?;
    let chr_pass = find_char(device, CHR_PASS)?;
    let chr_control = find_char(device, CHR_CONTROL)?;
    let chr_status = find_char(device, CHR_STATUS)?;

    device
        .write(&chr_ssid, ssid.as_bytes(), WriteType::WithResponse)
        .await
        .map_err(err_str)?;
    device
        .write(&chr_pass, password.as_bytes(), WriteType::WithResponse)
        .await
        .map_err(err_str)?;
    device
        .write(&chr_control, &[0x01], WriteType::WithResponse)
        .await
        .map_err(err_str)?;

    // "ap" is not a failure: the SoftAP stays up while the station keeps
    // retrying the join, so the status reads "ap" until the join lands.
    // Poll for "sta" and give a patient verdict on timeout.
    let deadline = SystemTime::now() + Duration::from_secs(45);
    loop {
        tokio::time::sleep(Duration::from_secs(2)).await;
        let raw = device.read(&chr_status).await.map_err(err_str)?;
        let status: WifiStatus =
            serde_json::from_slice(&raw).map_err(err_str)?;
        if status.state == "sta" {
            return Ok(status);
        }
        if SystemTime::now() > deadline {
            return Err(format!(
                "The device has not joined \"{ssid}\" yet. It keeps trying \
                 in the background. If it never appears, check the network \
                 name and password and send them again."
            ));
        }
    }
}

#[cfg_attr(mobile, tauri::mobile_entry_point)]
pub fn run() {
    tauri::Builder::default()
        .plugin(tauri_plugin_opener::init())
        .invoke_handler(tauri::generate_handler![
            device_info,
            sync_device,
            list_local_media,
            open_media,
            provision_wifi,
            backup_to_cloud
        ])
        .run(tauri::generate_context!())
        .expect("error while running tauri application");
}
