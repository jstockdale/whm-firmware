# whm-monitor 0.4.0 - THE WORLD (x3 twin + telemetry column)
1. ./flash.sh                     (no ESP-IDF needed - esptool only;
   the script checks its own deps and tells you what to install)
   - or with IDF: idf.py -p /dev/ttyACM0 flash monitor
2. On the console:  wifi join "<ssid>" <password>
   (creds persist; the monitor rejoins on every boot)
3. Run 'fleet pattern walker' on the panels; watch [M] + glass.
Console: help | wifi join/clear/status | mon | stats |
         bright <0-255> (saved) | reboot

Building from source instead (bundle -> monitor/): one-time IDF setup:
    git clone --depth 1 -b v5.5.2 --recursive \
        https://github.com/espressif/esp-idf ~/esp-idf
    ~/esp-idf/install.sh esp32s3
    . ~/esp-idf/export.sh          # per shell, then: idf.py build
