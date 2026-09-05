# Momento Firmware

ESP-IDF 6.1 firmware for the Seeed XIAO ESP32-S3 Sense board. Captures photos and video clips with audio, stores to SD card.

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
| REC button | GPIO1 (active low) |
| CAM button | GPIO2 (active low) |
| Status LED | GPIO4 |
| I2C SDA | GPIO5 |
| I2C SCL | GPIO6 |

## Build & Flash

```bash
# Build
idf.py build

# Flash and monitor
idf.py -p /dev/tty.usbmodem2101 flash monitor

# Clean
idf.py clean
```

## Media Output

| Type | Format | Path | Notes |
|------|--------|------|-------|
| Photo | JPEG | `/sdcard/PHOTO_NNN.JPG` | UXGA (1600×1200) |
| Video | MJPEG AVI | `/sdcard/VID_NNN.AVI` | VGA (640×480) at 15 fps |
| Audio | WAV | `/sdcard/AUD_NNN.WAV` | 16 kHz, 16-bit mono |

Recordings stream to the SD card and stop at 5 minutes, on a REC press,
or on an SD write failure.

## Usage

1. **CAM button (GPIO2)**: Press to capture a single UXGA photo; hold 1.5 s to toggle Wi-Fi sync mode
2. **REC button (GPIO1)**: Press to start recording; press again to stop

LED indicates activity:
- 2 blinks at boot = ready
- Solid on = capture/write in progress

## Project Structure

```
apps/firmware/
├── CMakeLists.txt        # ESP-IDF project config
├── sdkconfig.defaults    # ESP32-S3 config
├── dependencies.lock     # Component manager lock
├── main/
│   ├── momento_main.c    # Entry point, button handling
│   ├── sd_card.c/h       # SD card mount
│   ├── mic.c/h           # I2S PDM microphone
│   ├── avi_writer.c/h    # Streaming MJPEG AVI writer
│   ├── wav_writer.c/h    # Streaming WAV audio writer
│   ├── wifi_sync.c/h     # Sync mode: AP+station Wi-Fi + HTTP file server
│   └── ble_prov.c/h      # BLE Wi-Fi credential provisioning
├── .devcontainer/        # Docker dev environment
└── .vscode/              # VSCode ESP-IDF settings
```

## Development Container

Open in VS Code with Dev Containers extension for ESP-IDF 6.1 environment.

## Troubleshooting

- **Camera init fails**: Check ribbon cable, ensure stable 3.3V power
- **SD mount fails**: Format as FAT32, check SPI wiring
- **Build errors**: Run `idf.py reconfigure` or delete `build/`
- **PSRAM allocation fails**: Check sdkconfig has `CONFIG_SPIRAM=y`

## Docs

- [CLAUDE.md](./CLAUDE.md) — Detailed conventions and architecture
- [ESP-IDF docs](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/)
- [esp32-camera](https://github.com/espressif/esp32-camera)
