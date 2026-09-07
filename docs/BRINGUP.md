# WHM Bring-up Firmware (Phase 0)

Hardware validation firmware for the synchronized LED matrix project: three
Waveshare **ESP32-S3-RGB-Matrix** driver boards, each driving an Adafruit
#3649-class **P2.5 64x64 (1/32-scan, HUB75E)** panel.

This firmware exists to answer, empirically, every question the Phase 1+
media/sync design depends on:

| Stage | Validates | Design dependency |
|---|---|---|
| 1 display | esp-hub75 under pure IDF, FM6126A init, pin map, double-buffer flips | entire render path |
| 2 wifi | STA + SoftAP modes, power-save-off, **TSF readable + drift in ppm** | the shared show clock |
| 3 sensors | I2C bus, QMI8658 IMU, SHTC3 T/RH, PCF85063 RTC | orientation, thermal derate, schedules |
| 4 sd | SDMMC 1-bit mount, real MB/s throughput | media streaming from card |
| 5 audio | ES8311 speaker path (amp on GPIO11), ES7210 dual-mic capture | playback + visualization |
| 6 patterns | orientation/color order, banding, ghosting, 60fps frame pacing jitter | pattern = future player loop |

Everything in `main/whm_board.h` was extracted from Waveshare's official
sources (their IDF example's sdkconfig + BSP headers, cross-checked against
their Arduino sketch and shipped firmware image header) - no guessed pins.

## Quickstart

Prerequisites: git, python3, and ESP-IDF v5.5.x. If you don't have IDF:

    git clone -b v5.5.2 --depth 1 --recurse-submodules --shallow-submodules \
        https://github.com/espressif/esp-idf.git "$HOME/esp-idf-v5.5.2"
    "$HOME/esp-idf-v5.5.2/install.sh" esp32s3

Then, per shell session:

    source "$HOME/esp-idf-v5.5.2/export.sh"

Build, flash, monitor:

    bash scripts/build.sh
    bash scripts/flash.sh          # autodetects the port if unambiguous

`scripts/build.sh` validates every prerequisite up front and prints install
hints instead of dying halfway. First build downloads the five managed
components (esp-hub75, qmi8658, shtc3, pcf85063a, esp_codec_dev) from the
Espressif registry; `dependencies.lock` pins what was resolved.

## Configuration - no rebuild needed

WiFi is controlled at runtime from the serial console (same USB-C port,
115200 baud, `whm>` prompt):

    wifi scan                       # list nearby networks
    wifi join MyNetwork MyPass      # persists to NVS + connects immediately
    wifi status                     # mode, ip, rssi, tsf, drift
    wifi ap                         # switch back to SoftAP mode
    wifi clear                      # forget saved credentials

Credential precedence: **NVS (console) > Kconfig defaults > SoftAP
fallback** (`WHM-BRINGUP` / `whitehat-matrix`). The Kconfig entries under
"WHM Bring-up Configuration" remain useful as compile-time defaults when
provisioning a fleet, but nothing requires them.

## Screens

The panel boots to a **home screen** - node name (see `name`), version,
IP/WiFi state, and SD/RTC/IMU/T health dots - then you drive with one button.
The full set is Waveshare's six default info pages (IMU, RTC, SD, SHTC3,
WiFi, Audio) reimagined, plus home/sys/life which they never designed:

    home    identity + clock/date + IP + health dots (boot default).
            Clock color = sync status: amber = NTP-vouched, slate = not.
    clock   dedicated clock face: big time, 1px-per-second bar (full 60 + flash at :00), day+date; dim
            until NTP-synced. The colon blinks on a 2s period phase-locked
            to the disciplined clock - synced units blink in unison, an
            unsynced unit visibly free-runs (ambient fleet sync gauge).
            Corner dot: green = synced, amber = waiting, red = stale.
    timer   countdown (default 3:00, `timer default` to change); runs
            across screens, beeps and grabs the screen on expiry;
            button = start/pause/resume, reset after done
    music   album art (JPEG APIC via the S3 ROM jpeg decoder) with
            title/artist marquee + progress; 2TAP = play/pause,
            HOLD = next track. Tracks live in /sdcard/media.
    stopwatch  counts up w/ smooth sub-second sweep; button =
            start/stop/reset

    clock   RTC time large + date + uptime
    level   live IMU bubble level (crosshair, rings, pitch/roll) - also the
            seed of the gravity demos on the roadmap
    env     temperature large + humidity bar
    net     mode/SSID, IP, RSSI bars, TSF drift in ppm
    sys     version, heaps, uptime
    sd      card status, capacity, used-space bar
    vu      live dual-mic level meters with peak hold
    life    Conway's Game of Life, age-colored (reseeds itself)

The serial console uses an in-house line editor (no terminal probing, so
it behaves identically under miniterm, picocom, or a raw socket): up/down
= 16-entry history, backspace, ctrl-c clears the line. Tab-completion is
not yet implemented.

Navigation grammar (one button, scales to apps):
**single press** = next screen, everywhere, always (navigation never traps).
**double tap** = the app control channel (timer start/pause, stopwatch
run/stop, music play/pause). **long hold** = app long-action where one
exists (timer/watch reset, music next), otherwise toggles auto-rotation.
Screens without a double handler act on single press instantly; app screens
wait ~320ms to disambiguate.
Boot behavior is settable: `screen bootmode auto` makes unattended installs
rotate from power-on; `screen bootmode home` (default) boots to the status
screen. Console equivalents: `screen next`, `screen level`, `screen auto`.
Test patterns live behind `pattern <name>`; the button escapes them back to
the home screen.

## Console

The REPL runs on the native USB-Serial-JTAG - deliberately: UART0's default
ESP32-S3 pins (GPIO43/44) are wired to I2S BCLK and SD CMD on this board, so
UART0 must stay untouched. Commands (`help` lists them):

    wifi ...        as above
    tsf             TSF value + drift-vs-crystal in ppm
    sensors         one-shot IMU / temp+RH / RTC
    sysinfo         heap, uptime, reset reason, display state
    sd info|bench|ls [path]
    audio test      tone burst + mic RMS, on demand
    brightness N    live + persisted (0-255)
    screen X        info screens: name | next | auto | list | bootmode home|auto
    name [id]       node identity: home screen + SoftAP SSID + future mDNS
                    hostname; mDNS-safe charset enforced; 'name clear' unsets
    pattern NAME    hold a validation pattern (corners, white, sweep, ...)
    log LEVEL       quiet the 5s telemetry with 'log warn' while typing
    timer ...       countdown; stop keeps custom duration, reset
                    restores default; expiry alarm repeats ~20s
    vol [n]         beep/chime/tone volume (NVS, default 85)
    chime / tone    play the boot chime / one note (see screens above)
    stopwatch ..    stopwatch control; no-arg = status
    text <msg...>   put a message on the panel (auto-size, marquee if long)
    ntp / tz        SNTP discipline for the RTC + POSIX timezone (STA-gated)

    sd format       full-card FAT format behind a numeric confirmation
                    code bound to the card's serial (no expiry; a swapped
                    card or wrong code cancels)
    sd unmount      release the card before pulling it; watcher remounts
                    on insert automatically (hot-swap, no reboot)
    reboot

`pattern corners` held while you eyeball orientation, and `pattern sweep`
for pacing stats, are the two you'll use most on the bench.

## What you should see

Serial monitor at 115200. Abridged healthy boot:

    ==== [0: inventory] ====
    whm_main: flash: 32 MB
    whm_main: PSRAM: 16 MB
    ==== [1: display] ====
    whm_disp: HUB75 up: 64x64, FM6126A init, 20MHz, double-buffered, depth=8
    (panel blinks green)
    ==== [2: wifi + TSF] ====
    whm_wifi: WiFi power save: DISABLED (required for TSF accuracy)
    whm_wifi: TSF=... offset=+3.2 ppm        <- see "Reading TSF" below
    ==== [3: i2c + sensors] ====
    whm_sens: i2c devices: 0x18 0x40 0x51 0x6B 0x70 (5 found; ...)
    ==== [4: sd card] ====
    whm_sd: bench: wrote 4096 KB @ ~1-8 MB/s, read @ ... headroom ...x
    ==== [5: audio] ====
    (1kHz tone from speaker) mic L: rms=... peak=...
    ==== [6: patterns + telemetry] ====
    whm_pat: pattern: corners (orientation/color-order check)
    ...

The corners pattern is the orientation contract: **red top-left, green
top-right, blue bottom-left, white bottom-right**, 1px white border fully
visible on all four edges. Anything else means rotation/mirroring/color-order
needs attention before Phase 1.

`sweep_bar` logs worst/average frame-deadline misses at a 60fps target -
that loop is structurally identical to the future clock-scheduled player, so
its jitter numbers are directly meaningful for the sync error budget.

## Reading TSF (stage 2)

The whole multi-panel sync design rests on the 802.11 TSF counter. The
observer task samples `esp_wifi_get_tsf_time()` every 2s and prints the
offset between TSF-elapsed and local-crystal-elapsed time in ppm.

- **STA mode:** that offset is (approximately) your AP's clock vs this
  board's crystal. A stable value in the +/- tens of ppm is healthy and IS
  the free clock discipline the design claims: WiFi is continuously
  correcting it at the radio layer.
- **SoftAP mode:** TSF is locally generated; expect ~0 ppm. The interesting
  measurement - STA-follows-our-SoftAP quality - is a Phase 2 two-board
  test (GPIO toggle at a target TSF value on both boards, scope the skew).
- `TSF: 0` means not associated / no beacon yet; it should clear within
  seconds of connecting.

## First-flash checklist

1. Panel plugged into HUB75 header, panel powered from its own 5V supply
   (not the logic board's USB). Board USB-C to your machine.
2. Blank/garbage display but clean logs -> flip `FM6126A` to `GENERIC` in
   `main/display_hal.cpp` (one line). The Waveshare demo uses FM6126A for
   these panels, so FM6126A is expected correct.
3. Colors in wrong corners -> note which, we fix mapping in config, not
   by rewiring.
4. Watch supply current during the announced solid-white window.
5. No SD card is fine - stage 4 logs a warning and moves on.
6. Audio test needs the speaker plugged into the speaker header.

## Pin map (from vendor sources - do not edit casually)

    HUB75: R1=4 G1=5 B1=6 R2=7 G2=15 B2=16 A=18 B=8 C=3 D=42 E=9
           LAT=40 OE=2 CLK=41   (via 74HC245; clkphase=false)
    I2C:   SDA=47 SCL=48  (QMI8658 0x6B, SHTC3 0x70, PCF85063 0x51,
                           ES8311 0x18, ES7210 0x40)
    I2S:   MCLK=12 BCLK=43 WS=38 DOUT=21(ES8311) DIN=39(ES7210), PA_EN=11
    SD:    SDMMC 1-bit - CLK=1 CMD=44 D0=17, mounted at /sdcard

## Layout

    main/whm_board.h      single source of truth for wiring
    main/display_hal.*    C bridge over esphome/esp-hub75
    main/patterns.c       validation patterns + pacing stats
    main/storage_test.c   SDMMC mount + throughput bench
    main/sensors_test.c   I2C scan, IMU, T/RH, RTC
    main/audio_test.c     ES8311 tone + ES7210 RMS meter
    main/wifi_tsf.c       STA/SoftAP + TSF observer
    main/bringup_main.c   staged orchestrator

## Next (Phase 1)

Single-node media player: `.whm` raw container + GIF/PNG/JPEG from SD,
playlist, thin web UI, gamma + power governor. The display/storage/clock
numbers this firmware prints are its inputs.
