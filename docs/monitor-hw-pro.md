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

## ADDENDUM 2026-09-09: MVSRLora variant CONFIRMED (owner's unit)
## Extracted from Xinyuan-LilyGO/T-Display-S3-Pro-MVSRLora (GPL-3)

Radio: **Semtech LR1121** (datasheet LR1121_H2_DS_v2_0.pdf ships
in-repo) - multi-band: sub-GHz (150-960 MHz, so 915 for us) AND
2.4 GHz LoRa. Vendor examples run RadioLib 7.0.2 and demonstrate
both bands (868.1 vs 2400.1 in Deep_Sleep_Wake_Up).

### Pins (shared SPI with TFT/SD: SCLK 18 / MOSI 17 / MISO 8)
| LR1121 | GPIO | note |
|---|---|---|
| CS   | 7  | was CAMERA_VSYNC on base Pro |
| BUSY | 46 | was CAMERA_PWDN - **mandatory handshake pin** |
| INT  | 40 | LR11xx DIO9 IRQ; was CAMERA_Y4 |
| RST  | 10 | was CAMERA_Y8 |

**THE CAMERA SACRIFICE**: every radio pin reuses camera wiring -
the MVSRLora trades the camera port for the LR1121. The monitor
never wanted the camera; we lose nothing.

**CORRECTION** to the base-Pro section above: this repo's pin
table shows **TOUCH INT = IO21** - so 21 is the touch interrupt
on this family, not (only) the motion sensor. Poll still fine;
IRQ available if wanted.

### RF switch - DIO-controlled, table VERBATIM from vendor
DIO5/DIO6 drive the switch (DIO7/8/10 NC):
    STBY  LOW,LOW | RX HIGH,LOW | TX LOW,HIGH
    TX_HP LOW,HIGH | **TX_HF LOW,LOW** | GNSS/WIFI LOW,LOW
TX_HF (the 2.4 GHz path) at LOW,LOW routes around the sub-GHz PA
switch entirely. This table feeds SetDioAsRfSwitch at bring-up.
TCXO config: not seen in the example head - verify begin()
parameters at port time before assuming crystal.

### Driver plan (IDF-native, no Arduino)
LR11xx speaks a COMMAND protocol over SPI with the BUSY law:
assert NSS only when BUSY is low; every command waits BUSY.
v1 = a thin ~300-line command layer (opcodes from the in-repo
datasheet; Semtech SWDR001 as reference): GetVersion smoke,
SetPacketType(LoRa), SetRfFrequency, SetPaConfig/SetTxParams
per band, SetDioAsRfSwitch(table above), WriteBuffer + SetTx.
RadioLib consulted for sequencing knowledge only.

### Gateway, now concrete (MON_HAS_LORA = confirmed hardware)
- 915 MHz: the kilometer-haul type-32 digest at 1-2 Hz (pose,
  cam, rung, anchor/tsf echo) - duty-cycle friendly.
- 2.4 GHz LoRa: the worldwide-ISM fat pipe - near-field option
  for full 30 Hz keyframe relay or multi-digest bursts where
  sub-GHz airtime rules would pinch. Same frame format, band
  chosen per deployment; the RF-switch table makes swapping a
  one-command affair.
- Bring-up order stands: display first light on the Pro FIRST
  (boards/ refactor), radio second (GetVersion -> TX digest).
