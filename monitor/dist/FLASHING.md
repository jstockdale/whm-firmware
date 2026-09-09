# whm-monitor 0.2.0 - FIRST LIGHT (display + touch + live data)
1. Edit main/mon_config.h (SSID/password).
2. source <esp-idf>/export.sh
3. idf.py -p /dev/ttyACM0 flash monitor
Expect: got IP, then a 1 Hz [M] line with live step/state once
`fleet pattern walker` runs. Display bring-up is stage C.
