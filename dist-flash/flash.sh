#!/usr/bin/env bash
# WHM fleet panel flasher - no ESP-IDF required, esptool only.
# OTA layout: bootloader 0x0, partition table 0x8000,
#             ota_data 0x10000, app 0x20000.
# ==================== EDIT IF NEEDED ====================
PORT="${PORT:-/dev/ttyACM0}"
BAUD=921600
# ========================================================
cd "$(dirname "$0")"
fail() { echo; echo "!! $1"; echo "   $2"; exit 1; }

command -v python3 >/dev/null 2>&1 || \
  fail "python3 not found" "install: sudo apt install python3"

if ! python3 -m esptool version >/dev/null 2>&1; then
  echo "esptool (python module) not found. Install with ONE of:"
  echo "  sudo apt install esptool"
  echo "  python3 -m pip install --user --break-system-packages esptool"
  exit 1
fi

for f in bootloader.bin partition-table.bin ota_data_initial.bin \
         whm_bringup.bin; do
  [ -f "$f" ] || fail "$f missing" \
    "run this script inside the extracted whm-flash dir"
done

if [ ! -e "$PORT" ]; then
  echo "!! $PORT not found. Candidates:"
  ls /dev/ttyACM* /dev/ttyUSB* 2>/dev/null || echo "   (none visible)"
  echo "   pick one:  PORT=/dev/ttyACM1 ./flash.sh"
  echo "   permission denied later? sudo usermod -aG dialout \$USER"
  echo "   (then log out/in)"
  exit 1
fi

echo "== flashing WHM panel via $PORT =="
python3 -m esptool --chip esp32s3 -p "$PORT" -b "$BAUD" \
  --before default_reset --after hard_reset write_flash \
  0x0 bootloader.bin 0x8000 partition-table.bin \
  0x10000 ota_data_initial.bin 0x20000 whm_bringup.bin || \
  fail "esptool failed" "check the port, cable, and dialout group"

echo
echo "== done. console (115200): =="
echo "  python3 -m serial.tools.miniterm $PORT 115200"
echo "     (quit: ctrl-])  pyserial: sudo apt install python3-serial"
echo "Flash BOTH panels, then on either console:"
echo "  fleet pattern walker"
