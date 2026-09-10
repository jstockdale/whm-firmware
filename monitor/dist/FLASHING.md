# whm-monitor 0.5.0 - the Eye + the Relay
1. ./flash.sh            (esptool only; self-checks deps)
2. Console: wifi join "<ssid>" <password>   (persists)
Endpoints once joined (IP printed at boot / 'wifi status'):
  http://<ip>/          the Eye - atomic framebuffer capture (BMP)
  http://<ip>/viewer    full fleet viewer, served from flash
  ws://<ip>:8777        bridge relay (viewer connects itself)
Console: help | wifi ... | mon | stats | snap (base64 fb ->
  tools/whm_mon_snap.py) | tz <+-min> | bright <0-255> | reboot
