# whm-monitor 0.3.0 - console + NVS wifi (flash-and-go)
1. idf.py -p /dev/ttyACM0 flash monitor      (no source edits!)
2. On the console:  wifi join "<ssid>" <password>
   (creds persist; the monitor rejoins on every boot)
3. Run 'fleet pattern walker' on the panels; watch [M] + glass.
Console: help | wifi join/clear/status | mon | stats |
         bright <0-255> (saved) | reboot
