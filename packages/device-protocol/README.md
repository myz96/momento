# Momento Device Sync Protocol v1

This document is the contract between the firmware and the companion app.
The firmware implements the server side (`apps/firmware/main/wifi_sync.c`).
The app implements the client side (`apps/mobile/src-tauri/src/lib.rs`).

## Transport

The device runs a Wi-Fi access point and an HTTP server in "sync mode".

| Item | Value |
|------|-------|
| Trigger | Hold the CAM button for 1.5 s (hold again to exit) |
| SSID | `Momento` |
| Password | `momento123` |
| Base URL | `http://192.168.4.1` |
| Indicator | The status LED blinks while sync mode is on |

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
  { "name": "PHOTO_001.JPG", "size": 123456 },
  { "name": "AUD_001.WAV", "size": 456789 }
]
```

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

## Future (phase 2)

- BLE for discovery and pairing, so the app can switch Wi-Fi automatically.
- App → backend upload for cloud storage and AI analysis.
