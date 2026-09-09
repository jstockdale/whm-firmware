# Third-party components and licenses

This project (first-party code) is licensed **GPL-3.0-or-later** (see `LICENSE`).
All third-party code below is under permissive terms, one-way compatible with
GPLv3 today and with a possible future **BSD-3-Clause** relicense of the
first-party code. **No copyleft exists anywhere in the link graph.** The only
obligation either way is notice preservation.

## Vendored into `main/` (compiled into the binary)

| Files | Upstream | License (SPDX) | GPLv3 now | BSD-3 later |
|---|---|---|---|---|
| `main/monocypher.{c,h}` (4.0.2) | monocypher.org | `BSD-2-Clause OR CC0-1.0` | yes | yes (keep notice) |
| `main/minimp3.h` | github.com/lieff/minimp3 | `CC0-1.0` | yes | yes |

## Managed components (linked)

| Component | Role | License | GPLv3 now | BSD-3 later |
|---|---|---|---|---|
| `esphome__esp-hub75` | HUB75 LCD_CAM/GDMA panel driver | MIT | yes | yes |
| `espressif__esp_codec_dev` | ES8311/ES7210 codec glue | Apache-2.0 | yes | yes |
| `espressif__mdns` | mDNS responder | Apache-2.0 | yes | yes |
| `pedrominatel__shtc3` | SHTC3 T/RH sensor | Apache-2.0 | yes | yes |
| `waveshare__pcf85063a` | PCF85063 RTC | Apache-2.0 | yes | yes |
| `waveshare__qmi8658` | QMI8658 IMU | Apache-2.0 | yes | yes |

## Platform / ROM

| Piece | License | Notes |
|---|---|---|
| ESP-IDF v5.5.2 (incl. mbedtls SHA-256) | Apache-2.0 | one-way into GPLv3; fine under BSD-3 |
| TJpgDec (`esp32s3/rom/tjpgd.h`) | ChaN permissive | ROM-resident; not distributed by us |

## First-party

Everything else in `main/` and `components/whlink/` is (c) 2026 John Stockdale,
`GPL-3.0-or-later`, SPDX-stamped per file. Sole-copyright status preserves the
right to relicense first-party code (e.g., to BSD-3-Clause) at any time;
vendored files retain their own licenses under either regime.
