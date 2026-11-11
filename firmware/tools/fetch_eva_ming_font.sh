#!/usr/bin/env bash
# Eva-Ming-SC — Matisse-derived face for UI masks (CC BY-NC 4.0, non-commercial).
# Source: https://github.com/kaonashi-tyc/Eva-Ming-Simplified/tree/main/data
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DEST="$ROOT/assets/fonts/Eva-Ming-SC-v0.1.otf"
URL="https://github.com/kaonashi-tyc/Eva-Ming-Simplified/raw/main/data/Eva-Ming-SC-v0.1.otf"
mkdir -p "$(dirname "$DEST")"
if [[ -f "$DEST" ]]; then
  echo "Already present: $DEST"
  exit 0
fi
curl -fsSL "$URL" -o "$DEST"
echo "Downloaded $DEST"
