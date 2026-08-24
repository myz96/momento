# Momento Monorepo

This is a polyglot monorepo for Momento — a camera/audio capture device with embedded firmware, mobile/desktop companion app, and cloud backend.

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
Tauri 2.0 cross-platform app for desktop (macOS/Windows/Linux) and mobile (iOS/Android). Rust backend with web frontend (framework TBD when scaffolded).

**Tech**: Tauri 2.0, Rust, web frontend
**Build**: Not yet scaffolded — see `apps/mobile/CLAUDE.md` for setup instructions
**Details**: See `apps/mobile/CLAUDE.md`

### Backend (`apps/backend/`)
Python FastAPI backend for cloud sync, user accounts, media processing. Not yet scaffolded.

**Tech**: Python 3.12, FastAPI, uv, pytest, ruff
**Build**: Not yet scaffolded — see `apps/backend/CLAUDE.md` for setup instructions
**Details**: See `apps/backend/CLAUDE.md`

## Shared Packages (`packages/`)
For cross-app contracts:
- Protocol definitions for firmware ↔ backend communication
- OpenAPI/protobuf specs
- Shared TypeScript types for mobile ↔ backend

Currently empty — add files here as shared contracts emerge.

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

The firmware communicates with the mobile app and backend. Protocol details will be documented in `packages/` when defined. Expected:
- Bluetooth Low Energy (BLE) for mobile app pairing
- Wi-Fi/HTTP for backend sync
- Media format conventions (JPEG, AVI, WAV)
