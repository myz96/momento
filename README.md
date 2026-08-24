# Momento

A camera/audio capture device with embedded firmware, mobile/desktop companion app, and cloud backend.

## Project Structure

```
momento/
├── apps/
│   ├── firmware/     # ESP-IDF firmware (ESP32-S3)
│   ├── mobile/       # Tauri mobile/desktop app
│   └── backend/      # Python backend API
├── packages/         # Shared contracts, protocols, types
├── CLAUDE.md         # AI agent context (monorepo overview)
├── opencode.json     # opencode config
└── justfile          # Cross-app task runner
```

## Quick Start

### Prerequisites

- **just**: `brew install just`
- **Firmware**: ESP-IDF 6.1 (or use the devcontainer)
- **Mobile**: Rust, Node.js, pnpm (when scaffolded)
- **Backend**: Python 3.12, uv (when scaffolded)

### Build Firmware

```bash
just firmware-build
```

### Flash to Device

```bash
just firmware-flash
```

### View All Tasks

```bash
just
```

## Sub-Apps

| App | Description | Status |
|-----|-------------|--------|
| [firmware](./apps/firmware/) | ESP-IDF firmware for Seeed XIAO ESP32-S3 Sense | Active |
| [mobile](./apps/mobile/) | Tauri cross-platform app | Not scaffolded |
| [backend](./apps/backend/) | Python FastAPI backend | Not scaffolded |

## Hardware

The firmware targets the **Seeed XIAO ESP32-S3 Sense**:
- ESP32-S3 @ 240 MHz
- 8 MB flash, 8 MB octal PSRAM
- OV2640/OV5640 camera
- I2S PDM MEMS microphone
- MicroSD card slot

## Media Output

- **Photos**: JPEG (UXGA 1600×1200) → `/sdcard/PHOTO_NNN.JPG`
- **Video**: MJPEG AVI (VGA 640×480 @ 15fps) → `/sdcard/VID_NNN.AVI`
- **Audio**: WAV (16 kHz, 16-bit mono) → `/sdcard/AUD_NNN.WAV`

## Development

### Using Devcontainer (Recommended for Firmware)

Open in VS Code with the Dev Containers extension. Provides ESP-IDF 6.1 environment.

### Using opencode/Claude Code

This repo includes `CLAUDE.md` files for AI agent context:
- Root `CLAUDE.md` — Monorepo overview, build order, shared concerns
- `apps/*/CLAUDE.md` — Sub-app specific conventions and commands

opencode is configured via `opencode.json` to auto-load all sub-app context files.

## License

MIT
