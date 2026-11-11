#!/usr/bin/env bash
# MacBoard companion firmware — build / flash AI Passport (ESP32-C3).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FW="${ROOT}/firmware"
IDF_HELPER="${ROOT}/../AI_Board/scripts/get_idf553.sh"
PORT="${ESPPORT:-/dev/cu.usbmodem21401}"

if [[ ! -f "${IDF_HELPER}" ]]; then
  echo "Missing IDF helper at ${IDF_HELPER}" >&2
  exit 1
fi
# shellcheck disable=SC1090
. "${IDF_HELPER}"

cd "${FW}"
echo "Firmware tree: ${FW}"

if [[ ! -f sdkconfig ]]; then
  idf.py set-target esp32c3
fi

idf.py build

if [[ "${1:-}" == "flash" || "${1:-}" == "flash-monitor" ]]; then
  if [[ ! -e "${PORT}" ]]; then
    echo "Serial port ${PORT} missing. Set ESPPORT=..." >&2
    ls /dev/cu.* 2>/dev/null || true
    exit 1
  fi
  if [[ "${1}" == "flash-monitor" ]]; then
    idf.py -p "${PORT}" flash monitor
  else
    idf.py -p "${PORT}" flash
  fi
fi
