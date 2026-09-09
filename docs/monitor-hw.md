# whm-monitor hardware spec - LilyGO T-Display-S3 AMOLED (TOUCH)
## Extracted 2026-09-09 from LilyGo-AMOLED-Series (MIT, (c) 2023 TTGO)
## Board: BOARD_AMOLED_191 / beginAMOLED_191(touch=true)

Status: hardware validated by owner (vendor factory firmware).
The first repo consulted was the NON-TOUCH one - two landmines it
would have hidden are recorded below in bold.

## Display
- RM67162 AMOLED, 240x536 native (we drive landscape 536x240),
  QSPI. Pins (per non-touch pins_config.h + Series RM67162_AMOLED
  struct - Series serves both variants from one table):
  CS=6 SCK=47 D0=18 D1=7 D2=48 D3=5 RST=17 TE=9
- QSPI clock: vendor runs 75 MHz. Full RGB565 frame ~257 KB ->
  ~7 ms/frame; 30 fps with margin. Frame freq/cmd-bit fields of
  RM67162_AMOLED to be read verbatim at port time.
- **GPIO 38 = PMIC ENABLE - drive HIGH before init or the panel
  stays dark.** (Non-touch repo labels this pin "LED".)
- Brightness: command 0x51, one byte level (Series setBrightness).
- Init: initSequence.cpp `rm67162_cmd` - 12 entries (page-select
  0xFE dance, sleep-out 0x80-delay flag, OVSS trims). Copy
  VERBATIM into monitor/ with MIT header at port time; TE (0x35)
  and scan direction (0x36) lines are present but commented in
  vendor - decide at bring-up.

## Touch
- CST816T, I2C: SDA=3 SCL=2 **IRQ=21** RST=-1 (shares panel reset
  domain). **On the non-touch board GPIO 21 is BUTTON_2** - the
  collision the wrong repo would have hidden.
- Vendor driver: TouchDrvCSTXXX via lewisxhe/SensorLib 0.2.4
  (MIT). For IDF we implement a minimal raw driver (typical
  CST816 address 0x15; single-point + gesture regs) - verify
  address on glass at bring-up.

## Everything else
- Buttons: ONE - GPIO 0 (BOOT). Touch replaces the second.
- VBAT sense: ADC on GPIO 4. No PMU on this board (pmu=NULL).
- framebuffer=false in vendor config (their lib streams); the
  monitor keeps its own PSRAM framebuffer regardless.
- MCU: ESP32-S3, 16 MB flash, 8 MB OPI PSRAM.

## Plan
A. Hardware checkout - DONE (owner, vendor factory).
B. monitor/ IDF 5.5.2 app: WiFi STA join, UDP :7777 rx, parsers
   for types 9/10/16 (WHM-LINK addendum is the spec), RM67162 on
   esp_lcd QSPI, CST816T raw driver, PSRAM framebuffer + blit.
C. Render v0.1: x3 world twin (384x192) + 152 px telemetry
   column; pure world fns + sprite lifted C-to-C VERBATIM from
   main/ui.c - the second native consumer (RSK rule-of-three).
D. Polish: tap-to-toggle views, brightness gestures, burn-in
   guards (1 px jitter, kf-silence dim, night-off), oracle dot.

## Licensing
LilyGo-AMOLED-Series: MIT (c) 2023 TTGO - init sequence and pin
tables to be reproduced with header + THIRD_PARTY.md entry at
port time. SensorLib consulted for register knowledge only; our
touch driver is a fresh minimal implementation.
