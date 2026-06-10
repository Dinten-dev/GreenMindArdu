#!/usr/bin/env bash
# ============================================================================
# GreenMind ESP32 Sensor — One-Liner Flash Script
# ============================================================================
#
# One-liner flash (macOS / Linux):
#   curl -fsSL https://raw.githubusercontent.com/Dinten-dev/GreenMindArdu/main/flash-sensor.sh | bash
#
# Or clone first and run:
#   git clone https://github.com/Dinten-dev/GreenMindArdu.git && cd GreenMindArdu && bash flash-sensor.sh
#
# What this does:
#   1. Installs arduino-cli + PlatformIO if missing
#   2. Installs ESP32 board support + required libraries
#   3. Auto-detects the connected ESP32 USB port
#   4. Lets you choose the firmware variant
#   5. Compiles and flashes in one go
#   6. Opens serial monitor for verification
#
# Supported firmware variants:
#   1) GreenMindFirmware         — Production (ESP32-WROOM, Arduino)
#   2) GreenMindFirmware_AD8232  — Biosignal R&D (ESP32-WROOM, Arduino)
#   3) GreenMindFirmware_Biolingo — Custom PCB (ESP32-S3, PlatformIO)
#   4) GreenMindFirmware_OTA     — OTA-enabled (ESP32-WROOM, PlatformIO)
#
# Requirements:
#   - macOS or Linux (Debian/Ubuntu)
#   - USB-C cable connected to the ESP32
#   - Internet connection (first run only — for toolchain download)
# ============================================================================

set -euo pipefail

# ── Constants ────────────────────────────────────────────────────────────────

readonly VERSION="1.0.0"
readonly REPO_URL="https://github.com/Dinten-dev/GreenMindArdu.git"
readonly REPO_BRANCH="main"
readonly ESP32_CORE_URL="https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json"
readonly FQBN_WROOM="esp32:esp32:esp32"
readonly FQBN_S3="esp32:esp32:esp32s3"
readonly BAUD_RATE="115200"

# ── Colors ───────────────────────────────────────────────────────────────────

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
BOLD='\033[1m'
DIM='\033[2m'
NC='\033[0m'

# ── Helper Functions ─────────────────────────────────────────────────────────

info()    { echo -e "${BLUE}ℹ ${NC} $*"; }
success() { echo -e "${GREEN}✅${NC} $*"; }
warn()    { echo -e "${YELLOW}⚠️ ${NC} $*"; }
error()   { echo -e "${RED}❌${NC} $*" >&2; }
step()    { echo -e "\n${CYAN}${BOLD}── $* ──${NC}"; }

die() {
    error "$*"
    echo -e "${RED}Flash aborted.${NC}" >&2
    exit 1
}

# ── Banner ───────────────────────────────────────────────────────────────────

banner() {
    echo ""
    echo -e "${GREEN}${BOLD}"
    echo "  ╔══════════════════════════════════════════════════════╗"
    echo "  ║                                                      ║"
    echo "  ║   🌿 GreenMind ESP32 Flash Tool v${VERSION}             ║"
    echo "  ║                                                      ║"
    echo "  ║   Plug in your ESP32 and flash in seconds            ║"
    echo "  ║   https://green-mind.ch                               ║"
    echo "  ║                                                      ║"
    echo "  ╚══════════════════════════════════════════════════════╝"
    echo -e "${NC}"
}

# ── OS Detection ─────────────────────────────────────────────────────────────

detect_os() {
    case "$(uname -s)" in
        Darwin) OS="macos" ;;
        Linux)  OS="linux" ;;
        *)      die "Unsupported OS: $(uname -s). This script supports macOS and Linux." ;;
    esac
    info "Operating system: ${OS}"
}

# ── Step 1: Get Source Code ──────────────────────────────────────────────────

get_source() {
    step "1/6 — Source Code"

    # Check if we're already inside the repo
    if [ -f "GreenMindFirmware/GreenMindFirmware.ino" ]; then
        REPO_DIR="$(pwd)"
        success "Already in GreenMindArdu repository"
        return
    fi

    # Check if repo exists as a subdirectory
    if [ -d "GreenMindArdu" ] && [ -f "GreenMindArdu/GreenMindFirmware/GreenMindFirmware.ino" ]; then
        REPO_DIR="$(pwd)/GreenMindArdu"
        success "Found GreenMindArdu in current directory"
        return
    fi

    # Clone to a temporary location
    local clone_dir="${TMPDIR:-/tmp}/greenmind-ardu-$$"
    info "Cloning firmware repository..."
    git clone --branch "${REPO_BRANCH}" --depth 1 --quiet "${REPO_URL}" "${clone_dir}"
    REPO_DIR="${clone_dir}"
    CLEANUP_DIR="${clone_dir}"
    success "Repository cloned to ${clone_dir}"
}

# ── Step 2: Install Toolchains ───────────────────────────────────────────────

install_arduino_cli() {
    if command -v arduino-cli &>/dev/null; then
        success "arduino-cli already installed: $(arduino-cli version 2>&1 | head -1)"
        return
    fi

    info "Installing arduino-cli..."
    if [ "${OS}" = "macos" ]; then
        if command -v brew &>/dev/null; then
            brew install arduino-cli 2>/dev/null
        else
            curl -fsSL https://raw.githubusercontent.com/arduino/arduino-cli/master/install.sh | BINDIR="${HOME}/.local/bin" sh
            export PATH="${HOME}/.local/bin:${PATH}"
        fi
    else
        curl -fsSL https://raw.githubusercontent.com/arduino/arduino-cli/master/install.sh | BINDIR="${HOME}/.local/bin" sh
        export PATH="${HOME}/.local/bin:${PATH}"
    fi

    if ! command -v arduino-cli &>/dev/null; then
        die "Failed to install arduino-cli"
    fi
    success "arduino-cli installed"
}

install_platformio() {
    if command -v pio &>/dev/null; then
        success "PlatformIO already installed: $(pio --version 2>&1)"
        return
    fi

    info "Installing PlatformIO CLI..."
    if command -v pip3 &>/dev/null; then
        pip3 install --user platformio --quiet 2>/dev/null
    elif command -v pip &>/dev/null; then
        pip install --user platformio --quiet 2>/dev/null
    else
        curl -fsSL -o /tmp/get-platformio.py https://raw.githubusercontent.com/platformio/platformio-core-installer/master/get-platformio.py
        python3 /tmp/get-platformio.py
        rm -f /tmp/get-platformio.py
    fi

    # Add common PlatformIO paths
    export PATH="${HOME}/.local/bin:${HOME}/.platformio/penv/bin:${PATH}"

    if ! command -v pio &>/dev/null; then
        die "Failed to install PlatformIO. Install manually: pip3 install platformio"
    fi
    success "PlatformIO installed"
}

install_toolchains() {
    step "2/6 — Toolchain Setup"

    # We always need arduino-cli for the Arduino variants
    install_arduino_cli

    # Install ESP32 board support for arduino-cli
    info "Configuring ESP32 board support..."
    arduino-cli config init --overwrite 2>/dev/null || true
    arduino-cli config add board_manager.additional_urls "${ESP32_CORE_URL}" 2>/dev/null || true
    arduino-cli core update-index --quiet 2>/dev/null

    if arduino-cli core list 2>/dev/null | grep -q "esp32:esp32"; then
        info "ESP32 core already installed, updating..."
        arduino-cli core upgrade esp32:esp32 --quiet 2>/dev/null || true
    else
        info "Installing ESP32 core (this may take a few minutes on first run)..."
        arduino-cli core install esp32:esp32 --quiet
    fi

    # Install required Arduino libraries
    info "Installing Arduino libraries..."
    arduino-cli lib install "ArduinoJson@^7.0.0" --quiet 2>/dev/null || true

    success "Arduino toolchain ready"
}

# ── Step 3: Detect USB Port ──────────────────────────────────────────────────

detect_port() {
    step "3/6 — USB Port Detection"

    info "Scanning for connected ESP32 devices..."

    local ports=()

    if [ "${OS}" = "macos" ]; then
        # macOS: look for common ESP32 USB-serial devices
        while IFS= read -r port; do
            ports+=("${port}")
        done < <(ls /dev/cu.usbserial-* /dev/cu.usbmodem* /dev/cu.wchusbserial* /dev/cu.SLAB_USBtoUART* 2>/dev/null || true)
    else
        # Linux: look for common ESP32 USB-serial devices
        while IFS= read -r port; do
            ports+=("${port}")
        done < <(ls /dev/ttyUSB* /dev/ttyACM* 2>/dev/null || true)
    fi

    if [ ${#ports[@]} -eq 0 ]; then
        echo ""
        error "No ESP32 device found!"
        echo ""
        echo -e "${YELLOW}Troubleshooting:${NC}"
        echo "  1. Plug in your ESP32 via USB-C"
        echo "  2. Make sure you're using a data cable (not charge-only)"
        echo "  3. Check if a USB-serial driver is needed:"
        if [ "${OS}" = "macos" ]; then
            echo "     - CP2102:  brew install --cask silicon-labs-vcp-driver"
            echo "     - CH340:   brew install --cask wch-ch34x-usb-serial-driver"
        else
            echo "     - CP2102:  sudo apt install -y python3-serial"
            echo "     - CH340:   usually included in kernel"
        fi
        echo "  4. Try a different USB port or cable"
        echo ""
        die "No ESP32 detected. Connect your device and try again."
    fi

    if [ ${#ports[@]} -eq 1 ]; then
        PORT="${ports[0]}"
        success "Auto-detected ESP32 on: ${PORT}"
    else
        echo ""
        echo -e "${CYAN}Multiple USB devices found:${NC}"
        for i in "${!ports[@]}"; do
            echo "  $((i + 1))) ${ports[$i]}"
        done
        echo ""
        read -r -p "Select port [1-${#ports[@]}]: " choice
        if [[ ! "${choice}" =~ ^[0-9]+$ ]] || [ "${choice}" -lt 1 ] || [ "${choice}" -gt ${#ports[@]} ]; then
            die "Invalid selection"
        fi
        PORT="${ports[$((choice - 1))]}"
        success "Selected: ${PORT}"
    fi
}

# ── Step 4: Choose Firmware ──────────────────────────────────────────────────

choose_firmware() {
    step "4/6 — Firmware Selection"

    echo ""
    echo -e "${BOLD}Available firmware:${NC}"
    echo ""
    echo -e "  ${GREEN}1)${NC} ${BOLD}GreenMindFirmware_Biolingo${NC} — ESP32-S3 Biolingo v22"
    echo -e "     ${DIM}OLED display, OTA updates, AD8232 artifact detection, 380 Hz streaming${NC}"
    echo ""
    echo -e "     ${DIM}(Archived WROOM variants available in archive/ directory)${NC}"
    echo ""
    read -r -p "Select firmware [1] (default: 1): " fw_choice
    fw_choice="${fw_choice:-1}"

    case "${fw_choice}" in
        1)
            FIRMWARE="GreenMindFirmware_Biolingo"
            BUILD_SYSTEM="platformio"
            FQBN="${FQBN_S3}"
            BOARD_DESC="ESP32-S3 (Biolingo v22 PCB)"
            # PlatformIO needed for this variant
            install_platformio
            ;;

        *)
            die "Invalid selection: ${fw_choice}"
            ;;
    esac

    local fw_dir="${REPO_DIR}/${FIRMWARE}"
    if [ ! -d "${fw_dir}" ]; then
        die "Firmware directory not found: ${fw_dir}"
    fi

    success "Selected: ${FIRMWARE} (${BOARD_DESC})"
}

# ── Step 5: Compile & Flash ──────────────────────────────────────────────────

compile_and_flash() {
    step "5/6 — Compile & Flash"

    local fw_dir="${REPO_DIR}/${FIRMWARE}"

    if [ "${BUILD_SYSTEM}" = "arduino" ]; then
        compile_arduino "${fw_dir}"
    else
        compile_platformio "${fw_dir}"
    fi
}

compile_arduino() {
    local fw_dir="$1"
    local sketch_file
    sketch_file="$(find "${fw_dir}" -name '*.ino' -maxdepth 1 | head -1)"

    if [ -z "${sketch_file}" ]; then
        die "No .ino sketch found in ${fw_dir}"
    fi

    info "Compiling ${FIRMWARE} for ${FQBN}..."
    echo -e "${DIM}"
    arduino-cli compile \
        --fqbn "${FQBN}" \
        --build-property "build.extra_flags=-DCORE_DEBUG_LEVEL=0" \
        "${fw_dir}" 2>&1 | tail -5
    echo -e "${NC}"
    success "Compilation successful"

    info "Flashing to ${PORT}..."
    echo -e "${DIM}"
    arduino-cli upload \
        --fqbn "${FQBN}" \
        --port "${PORT}" \
        --upload-field "upload_speed=921600" \
        "${fw_dir}" 2>&1 | tail -5
    echo -e "${NC}"
    success "Firmware flashed successfully!"
}

compile_platformio() {
    local fw_dir="$1"

    # Override the upload port in platformio.ini
    info "Compiling ${FIRMWARE} with PlatformIO..."
    echo -e "${DIM}"
    pio run \
        --project-dir "${fw_dir}" \
        --target upload \
        --upload-port "${PORT}" 2>&1 | tail -10
    echo -e "${NC}"
    success "Firmware compiled and flashed!"
}

# ── Step 6: Verify ───────────────────────────────────────────────────────────

verify_flash() {
    step "6/6 — Verification"

    echo ""
    success "Flash complete! Your ESP32 is now running ${FIRMWARE}."
    echo ""

    echo -e "${BOLD}What happens next:${NC}"
    echo "────────────────────────────────────────"
    echo -e "  1. The ESP32 boots and enters ${YELLOW}Setup Mode${NC}"
    echo -e "  2. Connect to WiFi: ${GREEN}GreenMind-Sensor-XXXX${NC}"
    echo -e "  3. Open ${CYAN}http://192.168.4.1${NC} in your browser"
    echo -e "  4. Enter your WiFi credentials and pairing code"
    echo -e "  5. The sensor registers with the gateway and starts streaming"
    echo ""

    echo -e "${BOLD}Serial Monitor${NC}"
    echo "────────────────────────────────────────"
    echo -e "  Open the serial monitor to verify the boot:"
    echo ""
    if [ "${BUILD_SYSTEM}" = "arduino" ]; then
        echo -e "  ${CYAN}arduino-cli monitor -p ${PORT} -c baudrate=${BAUD_RATE}${NC}"
    else
        echo -e "  ${CYAN}pio device monitor --port ${PORT} --baud ${BAUD_RATE}${NC}"
    fi
    echo ""

    # Offer to open serial monitor
    read -r -p "Open serial monitor now? [Y/n] " open_monitor
    open_monitor="${open_monitor:-Y}"
    if [[ "${open_monitor}" =~ ^[Yy]$ ]]; then
        echo ""
        info "Opening serial monitor (Ctrl+C to exit)..."
        echo -e "${DIM}────────────────────────────────────────${NC}"
        if [ "${BUILD_SYSTEM}" = "arduino" ]; then
            arduino-cli monitor -p "${PORT}" -c baudrate="${BAUD_RATE}" || true
        else
            pio device monitor --port "${PORT}" --baud "${BAUD_RATE}" || true
        fi
    fi
}

# ── Cleanup ──────────────────────────────────────────────────────────────────

cleanup() {
    local exit_code=$?
    if [ -n "${CLEANUP_DIR:-}" ] && [ -d "${CLEANUP_DIR}" ]; then
        rm -rf "${CLEANUP_DIR}"
    fi
    if [ $exit_code -ne 0 ]; then
        echo ""
        error "Flash failed (exit code: ${exit_code})."
        echo -e "${YELLOW}Check the output above for details.${NC}"
    fi
}
trap cleanup EXIT

# ── Main ─────────────────────────────────────────────────────────────────────

main() {
    # Initialize
    REPO_DIR=""
    CLEANUP_DIR=""
    PORT=""
    FIRMWARE=""
    BUILD_SYSTEM=""
    FQBN=""
    BOARD_DESC=""

    banner
    detect_os
    get_source
    install_toolchains
    detect_port
    choose_firmware
    compile_and_flash
    verify_flash
}

main "$@"
