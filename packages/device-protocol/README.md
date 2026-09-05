# Momento Device Sync Protocol v1

This document is the contract between the firmware and the companion app.
The firmware implements the server side (`apps/firmware/main/wifi_sync.c`).
The app implements the client side (`apps/mobile/src-tauri/src/lib.rs`).

## Transport

Hold the CAM button for 1.5 s to toggle sync mode (the status LED blinks).
In sync mode the device runs an HTTP server, with the radio in AP+station
mode. Both paths are live at the same time:

1. **SoftAP** — always up for the whole session, as the reliable path.
2. **Station** — the device keeps retrying the home network stored in
   NVS (see "BLE provisioning" below) until the join lands, then it is
   also reachable as `momento.local` (mDNS) or `http://<device-ip>`.
   The retry pauses while a client is connected to the SoftAP. A join
   also sets the device clock over SNTP, which makes `mtime` real.

| SoftAP item | Value |
|------|-------|
| SSID | `Momento` |
| Password | `momento123` |
| Base URL | `http://192.168.4.1` |

## BLE provisioning

While sync mode is on, the device also advertises over BLE as `Momento`.
The app writes the home Wi-Fi credentials once; the device stores them in
NVS and reconnects in station mode.

GATT service `6D6F6D65-6E74-6F00-0000-000000000001`:

| Characteristic | UUID suffix | Access | Content |
|----------------|-------------|--------|---------|
| ssid | `…0002` | write | UTF-8 network name, 1–32 bytes |
| password | `…0003` | write | UTF-8 password, 0–64 bytes |
| control | `…0004` | write | `0x01` = save credentials and reconnect |
| status | `…0005` | read | JSON `{"state":"sta","ip":"192.168.1.7","ssid":"Home"}` |

`state` values: `sta` (on the home network; the SoftAP also stays up),
`ap` (SoftAP only; the station keeps retrying in the background), `off`.
Provisioning flow: write ssid → write password → write control `0x01` →
poll status every 2 s until `sta` or a client-side deadline (~45 s).
`ap` is NOT a failure signal — the join may simply not have landed yet,
and the device retries forever. Only a deadline expiry suggests wrong
credentials.

## Endpoints

### `GET /api/info`

Returns a summary of the device storage.

```json
{ "device": "momento", "files": 12, "total_bytes": 3456789 }
```

### `GET /api/files`

Returns the list of media files on the SD card. The list contains only
`.JPG`, `.JPEG`, `.WAV`, and `.AVI` files.

```json
[
  { "name": "PHOTO_001.JPG", "size": 123456, "mtime": 1787917687 },
  { "name": "AUD_001.WAV", "size": 456789, "mtime": 1787917694 }
]
```

`mtime` is the capture time in unix seconds, from the FAT timestamp.
The device has no battery-backed clock: values are only real after an
SNTP sync (station mode with internet) in the current power session.
Before that, FAT reports its 1980 epoch — clients must treat any value
before ~2021 as unknown and fall back to their own receive time.

### `GET /api/files/{name}`

Returns the file content. The response uses chunked transfer encoding, so
there is no `Content-Length` header. The `X-File-Size` header carries the
expected byte count. Content types: `image/jpeg`, `audio/wav`,
`video/x-msvideo`.

Errors: `400` for an invalid name, `404` when the file does not exist.

### `DELETE /api/files/{name}`

Removes one file from the SD card. The client calls this only after it
stored and verified the download.

```json
{ "deleted": true }
```

Errors: `400` for an invalid or non-media name, `404` when the file does
not exist, `500` when the delete fails.

## Sync flow (client side)

1. Join the `Momento` Wi-Fi network.
2. `GET /api/files` for the file list.
3. For each file:
   1. `GET /api/files/{name}` and stream to local storage.
   2. Compare the byte count with the listed `size`.
   3. On a match, `DELETE /api/files/{name}`.
   4. On a mismatch, delete the local copy and keep the file on the device.
4. The SD card is empty when every file transfers cleanly.

A per-file delete keeps the data safe: a lost connection mid-sync never
removes a file the app does not hold.

## Future

- BLE command to enter sync mode remotely, without the button hold.
- A hosted backend with object storage (the app → backend upload already
  exists against a local backend).
