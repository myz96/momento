use std::fs;
use std::path::{Path, PathBuf};
use std::time::{Duration, SystemTime, UNIX_EPOCH};

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

#[derive(Serialize, Deserialize)]
struct WifiStatus {
    state: String,
    ip: String,
    ssid: String,
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

    // The device restarts its Wi-Fi and joins the network; the join can
    // take up to ~25 s including the retry cycle.
    let deadline = SystemTime::now() + Duration::from_secs(40);
    loop {
        tokio::time::sleep(Duration::from_secs(2)).await;
        let raw = device.read(&chr_status).await.map_err(err_str)?;
        let status: WifiStatus =
            serde_json::from_slice(&raw).map_err(err_str)?;
        match status.state.as_str() {
            "sta" => return Ok(status),
            "ap" => {
                return Err(format!(
                    "The device could not join \"{ssid}\" and fell back to \
                     its own network. Check the network name and password."
                ))
            }
            _ => {}
        }
        if SystemTime::now() > deadline {
            return Err("Timed out while the device joined the network.".to_string());
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
            provision_wifi
        ])
        .run(tauri::generate_context!())
        .expect("error while running tauri application");
}
