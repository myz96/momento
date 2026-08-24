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

# Backend (placeholder until scaffolded)
backend-dev:
    @echo "Backend not yet scaffolded. See apps/backend/CLAUDE.md for instructions."

backend-test:
    @echo "Backend not yet scaffolded. See apps/backend/CLAUDE.md for instructions."

backend-lint:
    @echo "Backend not yet scaffolded. See apps/backend/CLAUDE.md for instructions."

# Mobile (placeholder until scaffolded)
mobile-dev:
    @echo "Mobile app not yet scaffolded. See apps/mobile/CLAUDE.md for instructions."

mobile-build:
    @echo "Mobile app not yet scaffolded. See apps/mobile/CLAUDE.md for instructions."

# All apps
lint:
    @echo "Run linters per sub-app (each manages its own tooling)"

test:
    @echo "Run tests per sub-app (each manages its own framework)"

# List tasks (default)
default:
    @just --list
