# Momento Mobile (Tauri)

Tauri 2.0 cross-platform app for desktop (macOS/Windows/Linux) and mobile (iOS/Android). Rust backend with web frontend.

## Status

**Not yet scaffolded.** This directory is a placeholder. Follow the setup instructions below when ready to start development.

## Tech Stack

- **Backend**: Rust via Tauri 2.0
- **Frontend**: Web (React/Vue/Svelte — choose when scaffolding)
- **Package Manager**: cargo (Rust) + pnpm (JS)
- **Build**: `cargo tauri dev` / `cargo tauri build`

## Scaffold Instructions

When ready to scaffold the mobile app:

```bash
cd apps/mobile

# Option A: Interactive scaffolding (recommended for first-time)
pnpm create tauri-app@latest .

# Option B: With specific frontend framework
pnpm create tauri-app@latest . --template react-ts
pnpm create tauri-app@latest . --template vue-ts
pnpm create tauri-app@latest . --template svelte-ts
```

After scaffolding, the structure will be:
```
apps/mobile/
├── src/                 # Web frontend source
├── src-tauri/           # Rust backend
│   ├── src/
│   │   └── main.rs      # Tauri entry point
│   ├── Cargo.toml
│   └── tauri.conf.json
├── package.json
└── CLAUDE.md
```

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

| Directory | Purpose |
|-----------|---------|
| `src/` | Web frontend (React/Vue/Svelte components, pages, state) |
| `src-tauri/src/` | Rust backend (Tauri commands, IPC handlers, native integrations) |
| `src-tauri/tauri.conf.json` | Tauri config (app name, windows, permissions) |

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

The mobile app will communicate with the Momento firmware device via:
- **BLE**: Device discovery, pairing, initial setup
- **Wi-Fi**: Media sync, firmware updates

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
