# whm-monitor 0.1.0 - headless wire proof
1. Edit main/mon_config.h (SSID/password).
2. source <esp-idf>/export.sh
3. idf.py -p /dev/ttyACM0 flash monitor
Expect: got IP, then a 1 Hz [M] line with live step/state once
`fleet pattern walker` runs. Display bring-up is stage C.
