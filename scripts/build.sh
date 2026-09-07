#!/usr/bin/env bash
# WHM bring-up: build script.
# Validates every prerequisite up front and fails fast with install hints.
# Run as a file (bash scripts/build.sh) from anywhere; it locates the project root.
set -euo pipefail

fail() { echo "ERROR: $*" >&2; exit 1; }

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
[ -f "$ROOT/CMakeLists.txt" ] || fail "project root not found (expected CMakeLists.txt next to scripts/)"

# ---- dependency validation, all up front ------------------------------------
command -v git >/dev/null 2>&1 || fail "git not found. Install: sudo apt install git"
command -v python3 >/dev/null 2>&1 || fail "python3 not found. Install: sudo apt install python3"
command -v cmake >/dev/null 2>&1 || fail "cmake not found. It ships with ESP-IDF's install.sh; see IDF hint below"

if ! command -v idf.py >/dev/null 2>&1; then
    cat >&2 <<'EOF'
ERROR: idf.py not found - ESP-IDF environment is not active.

Install ESP-IDF v5.5.x (one time):
    git clone -b v5.5.2 --depth 1 --recurse-submodules --shallow-submodules \
        https://github.com/espressif/esp-idf.git "$HOME/esp-idf-v5.5.2"
    "$HOME/esp-idf-v5.5.2/install.sh" esp32s3

Activate it (every shell session):
    source "$HOME/esp-idf-v5.5.2/export.sh"

Then re-run: bash scripts/build.sh
EOF
    exit 1
fi

IDF_VER_RAW="$(idf.py --version 2>/dev/null || true)"
case "$IDF_VER_RAW" in
    *v5.[3-9]*|*v[6-9].*) : ;;  # >= 5.3 required (managed components + i2c_master)
    *) fail "ESP-IDF >= v5.3 required, found: ${IDF_VER_RAW:-unknown}. Re-source the right export.sh" ;;
esac

echo "== deps OK: $IDF_VER_RAW"

# ---- build ------------------------------------------------------------------
cd "$ROOT"
if [ ! -f sdkconfig ]; then
    echo "== first build: setting target esp32s3 (applies sdkconfig.defaults)"
    idf.py set-target esp32s3
fi

idf.py build

echo
echo "== build OK. Artifacts:"
echo "   $ROOT/build/whm_bringup.bin"
echo "   $ROOT/build/bootloader/bootloader.bin"
echo "   $ROOT/build/partition_table/partition-table.bin"
echo "   $ROOT/build/ota_data_initial.bin"
echo
echo "Flash + monitor:  bash scripts/flash.sh [PORT]"
