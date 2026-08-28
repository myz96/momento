# Momento Monorepo Task Runner
# Run `just` to list all available tasks

# Firmware
firmware-build:
    cd apps/firmware && idf.py build

firmware-flash:
    cd apps/firmware && idf.py -p /dev/tty.usbmodem2101 flash monitor

firmware-monitor:
    cd apps/firmware && idf.py -p /dev/tty.usbmodem2101 monitor

firmware-clean:
    cd apps/firmware && idf.py clean

firmware-reconfigure:
    cd apps/firmware && idf.py reconfigure

# Backend
backend-dev:
    cd apps/backend && uv run uvicorn --app-dir src momento_backend.main:app --reload

backend-test:
    cd apps/backend && uv run pytest

backend-lint:
    cd apps/backend && uv run ruff check .

# Mobile
mobile-dev:
    cd apps/mobile && pnpm tauri dev

mobile-build:
    cd apps/mobile && pnpm tauri build

mobile-ios-dev:
    cd apps/mobile && pnpm tauri ios dev

# All apps
lint:
    @echo "Run linters per sub-app (each manages its own tooling)"

test:
    @echo "Run tests per sub-app (each manages its own framework)"

# List tasks (default)
default:
    @just --list
