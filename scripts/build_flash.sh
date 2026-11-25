#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
IDF_PATH="${IDF_PATH:-$HOME/esp/esp-idf}"
PORT="${ESPPORT:-/dev/ttyACM0}"
MONITOR=0

if [[ "${1:-}" == "--monitor" ]]; then
  MONITOR=1
fi

if [[ ! -f "$IDF_PATH/export.sh" ]]; then
  echo "ESP-IDF not found at $IDF_PATH"
  echo "Clone v5.5.1+ and run ./install.sh esp32s3"
  exit 1
fi

if [[ ! -r "$PORT" || ! -w "$PORT" ]]; then
  echo "Cannot access $PORT (permission denied)."
  echo "See docs/setup.md for the Linux udev rule, or run: sudo chmod 666 $PORT"
  exit 1
fi

# shellcheck disable=SC1091
source "$IDF_PATH/export.sh"

cd "$ROOT/firmware"
if [[ ! -f sdkconfig ]]; then
  idf.py set-target esp32s3
fi
idf.py build

if [[ "$MONITOR" -eq 1 ]]; then
  echo "Opening serial monitor (press Ctrl+] to exit)..."
  idf.py -p "$PORT" flash monitor
else
  idf.py -p "$PORT" flash
fi
