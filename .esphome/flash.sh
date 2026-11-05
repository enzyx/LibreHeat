#!/usr/bin/env bash
set -euo pipefail

# The configuration and secrets live one level above this script.
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.."

usage() {
  cat <<'EOF'
Usage: ./.esphome/flash.sh [DEVICE]
       ./.esphome/flash.sh --build-only

Build lg-wh27s.yaml into .esphome/build/lg-wh27s/ and flash it.
Without DEVICE, ESPHome prompts for a USB port or OTA destination.

Examples:
  ./.esphome/flash.sh /dev/ttyUSB0       # Build and flash over USB
  ./.esphome/flash.sh lg-wh27s.local     # Build and flash over Wi-Fi
  ./.esphome/flash.sh 192.168.1.123      # Build and flash over Wi-Fi by IP
  ./.esphome/flash.sh --build-only      # Build without flashing

Requires secrets.yaml and either uv or ESPHome 2026.10 or newer.
EOF
}

if [[ $# -gt 1 ]]; then
  usage >&2
  exit 2
fi

case "${1:-}" in
  -h|--help) usage; exit 0 ;;
  --build-only) action=compile ;;
  -*) usage >&2; exit 2 ;;
  *) action=run ;;
esac

if [[ ! -f secrets.yaml ]]; then
  echo 'Missing secrets.yaml. Copy secrets.example.yaml to secrets.yaml and fill in your credentials.' >&2
  exit 1
fi

if command -v uv >/dev/null 2>&1; then
  esphome_command=(uv tool run --from esphome==2026.10.2 esphome)
elif command -v esphome >/dev/null 2>&1; then
  esphome_command=(esphome)
else
  echo 'Install uv or ESPHome 2026.10 or newer first (see README.md).' >&2
  exit 1
fi

arguments=("$action" lg-wh27s.yaml)
if [[ "$action" == run ]]; then
  arguments+=(--no-logs)
  if [[ $# -eq 1 ]]; then
    arguments+=(--device "$1")
  fi
fi

"${esphome_command[@]}" "${arguments[@]}"
echo 'Firmware artifacts: .esphome/build/lg-wh27s/build/firmware.{ota,factory}.bin'
