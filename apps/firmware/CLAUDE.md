# Momento Firmware (ESP-IDF)

ESP-IDF 6.1 firmware for the Seeed XIAO ESP32-S3 Sense board mounted on the back of an iPhone via MagSafe. Captures photos and voice recordings throughout the day, storing them on an SD card for later sync to the companion app.

## Hardware

- **Board**: Seeed XIAO ESP32-S3 Sense
- **MCU**: ESP32-S3, 240 MHz, 8 MB flash, 8 MB octal PSRAM
- **Camera**: OV2640 or OV5640
- **Mic**: I2S PDM MEMS microphone (16 kHz, 16-bit mono)
- **Storage**: MicroSD card over SPI
- **Haptics**: DRV2605L on I2C (0x5A)

### Pin Map

| Function | GPIO |
|----------|------|
| CAM button | GPIO1 (active low) |
| REC button | GPIO2 (active low) |
| Status LED | GPIO4 |
| I2C SDA | GPIO5 |
| I2C SCL | GPIO6 |

Camera pins are defined in `momento_main.c` (CAM_PIN_* macros).

## User Interaction Model

The device is operated one-handed, often without looking at it.

| Button | Action | Haptic Pattern | Result |
|--------|--------|----------------|--------|
| **Shutter button** (GPIO1) | Single click | Short pulse | Capture one photo (JPEG) |
| **Shutter button** (GPIO1) | Hold 1.5 s | — | Toggle Wi-Fi sync mode (LED blinks) |
| **Audio button** (GPIO2) | Single click | Double pulse | Toggle start/stop audio recording (WAV) |

The haptic feedback is intentionally different between buttons so the user knows which one they pressed without looking.

> Video recording is not currently implemented.

## Wi-Fi Sync Mode

Hold the shutter button for 1.5 s to toggle sync mode. The status LED
blinks while sync mode is on. The device serves the HTTP file API and
picks a network in this order:

1. Station mode: joins the home network with NVS credentials, reachable
   as `momento.local` (mDNS).
2. SoftAP fallback: SSID `Momento`, password `momento123`, at
   `http://192.168.4.1`.

In sync mode the device also advertises over BLE as `Momento`
(`main/ble_prov.c`, NimBLE). The app sends the home Wi-Fi credentials
over BLE once; the device stores them in NVS and reconnects.

The full contract lives in `packages/device-protocol/README.md`.
Implementation: `main/wifi_sync.c` and `main/ble_prov.c`.

## Build & Flash

```bash
# Build
cd apps/firmware && idf.py build

# Flash and monitor (adjust port as needed)
idf.py -p /dev/tty.usbmodem2101 flash monitor

# Clean
idf.py clean
```

Or use the justfile from repo root:
```bash
just firmware-build
just firmware-flash
just firmware-monitor
```

## Project Structure

```
apps/firmware/
├── CMakeLists.txt        # ESP-IDF project config
├── sdkconfig.defaults    # ESP32-S3 config (8MB flash, octal PSRAM)
├── dependencies.lock     # compote lock file
├── main/
│   ├── momento_main.c    # Entry point, button handling, capture logic
│   ├── sd_card.c/h       # SD card mount and test
│   ├── mic.c/h           # I2S PDM microphone driver
│   ├── avi_writer.c/h    # MJPEG AVI file writer
│   ├── wav_writer.c/h    # WAV audio file writer
│   ├── wifi_sync.c/h     # Sync mode: station/SoftAP Wi-Fi + HTTP file server
│   └── ble_prov.c/h      # BLE Wi-Fi credential provisioning (NimBLE)
├── .devcontainer/       # Docker devcontainer for ESP-IDF
└── .vscode/             # VSCode settings for ESP-IDF extension
```

## Dependencies

Managed via ESP-IDF Component Manager (compote):
- `espressif/esp32-camera` ^2.0.0 — camera driver

See `main/idf_component.yml` and `dependencies.lock`.

## Media Output

| Type | Format | Path | Notes |
|------|--------|------|-------|
| Photo | JPEG | `/sdcard/PHOTO_NNN.JPG` | UXGA (1600×1200) |
| Video | MJPEG AVI | `/sdcard/VID_NNN.AVI` | VGA (640×480) at 15 fps |
| Audio | WAV | `/sdcard/AUD_NNN.WAV` | 16 kHz, 16-bit mono |

Recording stops at 30 seconds or when PSRAM buffer fills (~10-12s typical).

## C Coding Conventions

- Use `esp_err_t` for error returns, check with `ESP_OK`
- Log with `ESP_LOGI`, `ESP_LOGW`, `ESP_LOGE` (tag = module name)
- Use FreeRTOS primitives (SemaphoreHandle_t, TaskHandle_t) for concurrency
- Allocate large buffers in PSRAM: `heap_caps_malloc(size, MALLOC_CAP_SPIRAM)`
- Header guards: `#pragma once`

## Development Container

The `.devcontainer/` provides an ESP-IDF Docker environment:
- Base: `espressif/idf:latest`
- Extensions: ESP-IDF extension, ESP-IDF Web

Open in VS Code with the Dev Containers extension.

## Troubleshooting

- **Camera init fails**: Check ribbon cable connection, ensure 3.3V power stable
- **SD mount fails**: Card may need formatting (FAT32), or SPI wiring issue
- **Build errors**: Run `idf.py reconfigure` or delete `build/` directory
- **PSRAM allocation fails**: Ensure `CONFIG_SPIRAM=y` and `CONFIG_SPIRAM_MODE_OCT=y` in sdkconfig

## Related

- Root monorepo: `../../CLAUDE.md`
- ESP-IDF docs: https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/
- esp32-camera: https://github.com/espressif/esp32-camera
