# Momento Mobile (Tauri)

Tauri 2.0 app for macOS and iOS. Rust backend with a web frontend. Windows, Linux, and Android support are possible via Tauri but not current targets.

## Status

**Scaffolded, first feature built.** The app syncs media from the device over
Wi-Fi and shows it in a gallery. See "Device Sync" below.

## Tech Stack

- **Backend**: Rust via Tauri 2.0
- **Frontend**: React 19 + Vite + TypeScript
- **Package Manager**: cargo (Rust) + pnpm (JS)
- **Build**: `pnpm tauri dev` / `pnpm tauri build`

## Device Sync

The contract is in `packages/device-protocol/README.md`. The flow:

1. One-time setup: the user holds CAM for 1.5 s, then sends the home
   Wi-Fi credentials from the app over BLE (`provision_wifi`, btleplug).
   The device stores them and joins the home network as `momento.local`.
2. For a sync, the user holds CAM for 1.5 s and presses **Sync now**.
   Without setup, the device falls back to its own `Momento` network
   (password `momento123`, base `http://192.168.4.1`).
3. Rust command `sync_device` downloads each file, verifies the byte
   count, stores it under `$APPDATA/media/` with an epoch-ms prefix, then
   deletes it from the device. Progress streams to the UI over a
   `Channel`.
4. The gallery (`src/App.tsx`) lists local media: photos render inline,
   WAV files play in an `<audio>` element, AVI clips open in an external
   player via `open_media` (web views do not decode MJPEG AVI).

Rust commands live in `src-tauri/src/lib.rs`: `device_info`, `sync_device`,
`list_local_media`, `open_media`, `provision_wifi`. The asset protocol is
scoped to `$APPDATA/media/**` in `tauri.conf.json` so the webview can load
local media with `convertFileSrc`. `src-tauri/Info.plist` carries the
macOS/iOS Bluetooth and local-network usage descriptions.

## Core App Purpose

The companion app is the primary place to review, search, and reflect on captured moments.

Responsibilities:
- **Sync**: pull photos and voice recordings from the MagSafe device over BLE + Wi-Fi
- **Timeline**: browse a chronological feed of photos and audio clips
- **Review**: play voice memos, view photos, add notes or tags
- **AI assistance** (future): ask natural-language questions about your day, get summaries, find patterns
- **Cloud relay**: upload to the backend for long-term storage and deeper analysis (future)

## Development Commands

After scaffolding:

```bash
# Development server with hot reload
pnpm tauri dev

# Build for production
pnpm tauri build

# Build for specific platform
pnpm tauri build --target universal-apple-darwin  # macOS
pnpm tauri build --target aarch64-apple-ios       # iOS
```

Or use the justfile from repo root:
```bash
just mobile-dev
just mobile-build
```

## Project Structure (Expected)

| Directory                   | Purpose                                                          |
| --------------------------- | ---------------------------------------------------------------- |
| `src/`                      | Web frontend (React/Vue/Svelte components, pages, state)         |
| `src-tauri/src/`            | Rust backend (Tauri commands, IPC handlers, native integrations) |
| `src-tauri/tauri.conf.json` | Tauri config (app name, windows, permissions)                    |

## Tauri Commands (Rust)

Define commands in `src-tauri/src/main.rs`:

```rust
#[tauri::command]
fn my_command() -> String {
    "Hello from Rust".into()
}

fn main() {
    tauri::Builder::default()
        .invoke_handler(tauri::generate_handler![my_command])
        .run(tauri::generate_context!())
        .expect("error while running tauri application");
}
```

Call from frontend:
```typescript
import { invoke } from '@tauri-apps/api/core';
await invoke('my_command');
```

## Mobile-Specific Notes

### iOS
- Requires Xcode 15+ on macOS
- Run `pnpm tauri ios init` after scaffolding
- Build: `pnpm tauri ios build`
- Test on simulator: `pnpm tauri ios dev --target <simulator-id>`

### Android
- Requires Android SDK + NDK
- Run `pnpm tauri android init` after scaffolding
- Build: `pnpm tauri android build`
- Test on emulator: `pnpm tauri android dev`

## Integration with Firmware

The app pulls media from the Momento device. Primary sync is device → app; cloud is a future step.

- **BLE**: device discovery, pairing, lightweight status checks
- **Wi-Fi**: bulk media transfer (photos + audio files)

Protocol details will be defined in `packages/` (shared contracts).

## Integration with Backend

The mobile app will communicate with the backend API via:
- REST/GraphQL for user accounts, cloud sync
- WebSocket for real-time notifications

API contracts will be defined in `packages/` (OpenAPI spec).

## Rust Coding Conventions

- Use `#[tauri::command]` for all IPC handlers
- Return `Result<T, String>` for error handling
- Use `serde` for JSON serialization
- Keep commands focused — one responsibility per command
- Handle all errors explicitly, no `.unwrap()` in production code

## Frontend Coding Conventions

(Choose and document after selecting frontend framework)

## Related

- Root monorepo: `../../CLAUDE.md`
- Tauri docs: https://v2.tauri.app/
- Tauri 2.0 guide: https://v2.tauri.app/start/
