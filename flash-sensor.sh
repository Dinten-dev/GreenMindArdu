#!/usr/bin/env bash
set -Eeuo pipefail

usage() {
    cat <<'EOF'
Usage: ./flash-sensor.sh [--port DEVICE] [--monitor]

Build and flash the active ESP32-S3 firmware with an already installed,
version-compatible PlatformIO CLI. Run this script from a reviewed checkout;
it deliberately does not download or execute remote installers.

Options:
  --port DEVICE  Explicit serial device (otherwise PlatformIO auto-detects it)
  --monitor      Open the 115200-baud serial monitor after a successful flash
  --help         Show this help
EOF
}

upload_port=""
open_monitor=false

while [[ $# -gt 0 ]]; do
    case "$1" in
        --port)
            [[ $# -ge 2 ]] || { echo "--port requires a device" >&2; exit 2; }
            upload_port="$2"
            shift 2
            ;;
        --monitor)
            open_monitor=true
            shift
            ;;
        --help)
            usage
            exit 0
            ;;
        *)
            echo "Unknown option: $1" >&2
            usage >&2
            exit 2
            ;;
    esac
done

command -v pio >/dev/null 2>&1 || {
    echo "PlatformIO is required. Install the reviewed version documented in README.md." >&2
    exit 1
}

readonly script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
readonly project_dir="${script_dir}/GreenMindFirmware_Biolingo"

build_args=(
    run
    --project-dir "${project_dir}"
    --environment biolingo_v22
    --target upload
)
if [[ -n "${upload_port}" ]]; then
    build_args+=(--upload-port "${upload_port}")
fi

pio "${build_args[@]}"

if [[ "${open_monitor}" == true ]]; then
    monitor_args=(device monitor --baud 115200)
    if [[ -n "${upload_port}" ]]; then
        monitor_args+=(--port "${upload_port}")
    fi
    pio "${monitor_args[@]}"
fi
