# Momento Monorepo

This is a polyglot monorepo for Momento — a camera/audio capture device that mounts to the back of an iPhone via MagSafe. It captures photos and voice recordings throughout the day so users can review their moments later, with optional AI-assisted analysis for pattern detection and summarization.

The companion app runs on macOS and iOS (built with Tauri 2.0). The firmware lives on an ESP32-S3 with an SD card. Media sync is device → app first; cloud upload will come later.

## Project Structure

```
momento/
├── apps/
│   ├── firmware/     # ESP-IDF firmware (ESP32-S3)
│   ├── mobile/       # Tauri mobile/desktop app
│   └── backend/      # Python backend API
├── packages/         # Shared contracts, protocols, types
├── CLAUDE.md         # This file
├── opencode.json     # opencode config (instructions glob)
└── justfile          # Cross-app task runner
```

## Stack Overview

| App | Language | Framework | Package Manager | Build/Run |
|-----|----------|-----------|-----------------|-----------|
| firmware | C | ESP-IDF 6.1 | compote | `idf.py build` |
| mobile | Rust + TypeScript | Tauri 2.0 | cargo + pnpm | `cargo tauri dev` |
| backend | Python 3.12 | FastAPI | uv | `uv run uvicorn` |

## Sub-App Summaries

### Firmware (`apps/firmware/`)
ESP-IDF firmware for Seeed XIAO ESP32-S3 Sense board. Captures photos and video clips with audio, stores to SD card. Uses esp32-camera component.

**Tech**: ESP-IDF 6.1, ESP32-S3, OV2640/OV5640 camera, I2S MEMS mic, FAT/SDMMC
**Build**: `just firmware-build` or `cd apps/firmware && idf.py build`
**Details**: See `apps/firmware/CLAUDE.md`

### Mobile (`apps/mobile/`)
Tauri 2.0 app for macOS and iOS. Rust backend with a React frontend. Syncs media from the device over Wi-Fi and shows it in a gallery.

**Tech**: Tauri 2.0, Rust, React 19 + Vite + TypeScript
**Build**: `just mobile-dev` or `cd apps/mobile && pnpm tauri dev`
**Details**: See `apps/mobile/CLAUDE.md`

> Tauri supports Windows, Linux, and Android, but Momento targets macOS and iOS for now.

### Backend (`apps/backend/`)
Python FastAPI backend for cloud sync, user accounts, media processing. Scaffolded with a health route; cloud sync is a later phase.

**Tech**: Python 3.12, FastAPI, uv, pytest, ruff
**Build**: `just backend-dev` or `cd apps/backend && uv run uvicorn --app-dir src momento_backend.main:app --reload`
**Details**: See `apps/backend/CLAUDE.md`

## Shared Packages (`packages/`)
For cross-app contracts:
- `device-protocol/README.md` — the device ↔ app sync contract (Wi-Fi SoftAP + HTTP API)
- OpenAPI/protobuf specs (future)
- Shared TypeScript types for mobile ↔ backend (future)

## Task Runner (`justfile`)

Run `just` to see available tasks:
```
just                  # List all tasks
just firmware-build   # Build firmware
just firmware-flash   # Flash to device
just backend-dev      # Start backend (when scaffolded)
just mobile-dev       # Start mobile app (when scaffolded)
```

## Build Order

When all apps are scaffolded:
1. `packages/` contracts first (if generating code from specs)
2. `apps/backend/` 
3. `apps/mobile/`
4. `apps/firmware/` (independent, can build anytime)

## Cross-App Conventions

### Reading Sub-App CLAUDE.md Files
When working on cross-app tasks or needing detail about a specific sub-app:
- Firmware: `apps/firmware/CLAUDE.md`
- Mobile: `apps/mobile/CLAUDE.md`  
- Backend: `apps/backend/CLAUDE.md`

Use the Read tool to load these for detailed conventions, commands, and architecture specific to each sub-app.

### Environment Variables
Each sub-app manages its own `.env` file. Root-level secrets (if any shared) go in a root `.env` that sub-apps reference.

### Code Style
Each sub-app defines its own style in its CLAUDE.md. No cross-app style enforcement currently.

## Device Communication Protocol

The sync contract is documented in `packages/device-protocol/README.md`. Current state:
- **Wi-Fi**: the device runs a SoftAP + HTTP file server in sync mode; the app pulls files and clears the SD card
- **Bluetooth Low Energy (BLE)**: planned for discovery and pairing
- **Cloud sync**: app → backend will come later
- **Media formats**: JPEG (photos), WAV (audio), MJPEG AVI (clips)
