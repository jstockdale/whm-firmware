# whm-monitor hardware spec #2 - LilyGO T-Display-S3 Pro
## Extracted 2026-09-09 from T-Display-S3-Pro vendor repo
## Board: second monitor target (multi-board architecture)

## Display
- ST7796 IPS, 222x480 native (we drive landscape 480x222),
  CLASSIC SPI with DC line - not QSPI-DCS like the AMOLED:
  CS=39 DC=9 SCK=18 MOSI=17 MISO=8 RST=47
- **THE 49-COLUMN LANDMINE**: the vendor constructor is
  Arduino_ST7796(bus, RST, rot 0, IPS, 222, 480, **49**, 0) -
  the 222-wide window sits at GRAM column 49. In landscape
  (MADCTL MV) that offset moves to the ROW axis. Wrong gap =
  a 49 px black band and wrapped pixels. esp_lcd path:
  ST7789-compatible command set + panel_set_gap per rotation;
  init table cribbed verbatim from Arduino_GFX's ST7796
  source (MIT) at port time, with header + THIRD_PARTY entry.
- Backlight: **PWM on GPIO 48** (vendor LEDC_TFT_CH 1) - the
  brightness path forks: AMOLED speaks DCS 0x51, Pro speaks
  LEDC duty. 'bright <0-255>' maps to duty on this board.

## Touch
- **CST226SE at 0x5A** (CST226SE_SLAVE_ADDRESS), shared I2C
  SDA=5 SCL=6, RST=13. Different register map than the
  AMOLED's CST816T - second minimal raw backend (multi-point
  report at reg 0x00, 12-bit coords; crib from SensorLib's
  CST226 implementation, register knowledge only).
- BOARD_SENSOR_IRQ 21 is the MOTION sensor's IRQ on this
  board (BHI260 lives here too) - do not confuse with touch.

## Everything else
- ESP32-S3R8, 16MB flash, 8MB OPI PSRAM - same MCU class,
  same sdkconfig bones.
- THREE buttons: 0 (BOOT), 12, 16. SD on CS=14 (shared SPI).
- PMU: **SY6970** charger/power-path on I2C. The GFX hello
  example runs without PMU init, so the panel rail appears
  direct - VERIFY ON GLASS; if dark, SY6970 boost/OTG enable
  is the suspect (this board's possible GPIO-38 analogue).
- Camera port (multiple sensor options), BHI260AP, LTR553,
  ICM20948 - a loaded board; all ignored by the monitor.

## LoRa - the finding
This repo contains NO radio: the only 'lora' grep hits are
NeoPixel FLORA false-positives, and the README lists
T-Display-S3-Pro-**MVSRLora** as a SEPARATE product with its
own repository (all-dashes spec row here). Owner to confirm
which unit is on the desk (antenna connector = MVSRLora).

## The LoRa telemetry gateway (design, banked)
Behind a board-capability flag MON_HAS_LORA, built when the
MVSRLora pinmap is in hand:
- Mode: TX gateway. The monitor already hears everything
  passively; every N seconds it re-broadcasts a compact
  digest over sub-GHz: magic WHML, ver, type 32 (LORA
  DIGEST), step u32, x f32, y/st/owner, cam f32, latest rung
  step u32 + h16 - under 32 bytes, SF7-class airtime, duty-
  cycle respected (US915 for the owner).
- Far node: a second monitor in LORA-RX mode renders the
  world twin from 1-2 Hz digests (pose interpolated between
  digests; terrain is pure f(cam) so it needs nothing else).
- 'Sync more distant systems': digests carry anchor + a tsf
  echo, giving far nodes coarse (+-ms) fleet-clock discipline
  - enough for scenery phase and rung bookkeeping, not for
  sim lockstep. Observers stay free, now at kilometer scale.

## Multi-board architecture (the port plan)
monitor/main/boards/: one header per board (pins, LCD_W/H,
panel backend select, touch backend select, brightness path,
capability flags). Kconfig choice MON_BOARD ->
{AMOLED191 (default), S3PRO}; CMake picks
mon_lcd_rm67162.c|mon_lcd_st7796.c and
mon_touch_cst816.c|mon_touch_cst226.c. Everything above
whm_display_fill_rect - parsers, TSF slave, console, pages,
Eye, Relay, world renderer - is already board-agnostic;
render scales derive from LCD_W (Pro: world x3 = 384 + 92 px
column; FULL page = x3 full-bleed, 64 rows fit 222 with
vertical centering, no crop needed). Releases ship one
tarball per board from the Pro's first light onward.
