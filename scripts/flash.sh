#!/usr/bin/env bash
# WHM bring-up: flash + monitor (requires ESP-IDF environment, like build.sh).
# Usage: bash scripts/flash.sh [PORT]
# With no PORT, autodetects if exactly one candidate serial device exists.
set -euo pipefail

fail() { echo "ERROR: $*" >&2; exit 1; }

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
command -v idf.py >/dev/null 2>&1 || fail "idf.py not found - source ESP-IDF's export.sh first (see scripts/build.sh hint)"
[ -f "$ROOT/build/whm_bringup.bin" ] || fail "no build output found - run: bash scripts/build.sh"

PORT="${1:-}"
if [ -z "$PORT" ]; then
    mapfile -t CANDS < <(ls /dev/ttyACM* /dev/ttyUSB* /dev/cu.usbmodem* /dev/cu.usbserial* 2>/dev/null || true)
    if [ "${#CANDS[@]}" -eq 1 ]; then
        PORT="${CANDS[0]}"
        echo "== autodetected port: $PORT"
    elif [ "${#CANDS[@]}" -eq 0 ]; then
        fail "no serial device found. Plug the board in via USB-C (data cable), then re-run. If it still fails, hold BOOT while plugging in."
    else
        echo "Multiple serial devices found:" >&2
        printf '  %s\n' "${CANDS[@]}" >&2
        fail "specify one: bash scripts/flash.sh <PORT>"
    fi
fi

cd "$ROOT"
idf.py -p "$PORT" flash monitor
