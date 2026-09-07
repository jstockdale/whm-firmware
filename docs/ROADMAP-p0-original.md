# WHM Roadmap

## Phases (core system)

- **P0 - bring-up** - DONE in firmware (v0.3.0), pending bench validation.
  Display (esp-hub75/FM6126A), SDMMC bench, sensors, audio, WiFi + TSF
  observer, USB serial console with runtime WiFi control, 8 info screens.
- **P0.5 - shipped en route**: SNTP/TZ RTC discipline (recovered from an
  orphaned session branch, fully audited, owner-approved), proportional
  font with tabular digits, `text` display command, home-screen clock.
- **P1 - single-node media player** - `.whm` raw container spec, GIF/PNG/JPEG
  from SD, playlist, host converter tool (Python/ffmpeg), gamma + power
  governor, thin web UI (client-heavy).
- **P2a - DONE (0.8.0+)**: WHM-LINK core - roles, announce, fleet wall
  clock, mDNS + discover, anchor/follow infra mode. Design: WHM-LINK.md.
- **P2b - SHIPPED, seam verdict pending**: wanderer across the strip;
  fleet text (stationary-across-seam + TSF ticker); fleet LIFE (lockstep
  gens on the show clock, ghost-column exchange, deterministic epoch
  reseeds, horizontal-torus strip).
- **P2c - NEXT**: HTTP file service (manifest + ranged GET on every
  node) + `sync media` pull-sync; CMD fan-out (`fleet <console line>`) so
  fleet text/life/play become one command.
- **P2d**: Mode A synced audio - replicate + `PLAY{sha,start_tsf}` +
  the TSF sample-slaving engine (insert/drop on >500us error).
- **P2e**: Mode B live DJ streaming - PTS-stamped MP3 frames over
  per-member TCP into the same engine.
- **P3**: DDP / ArtNet / sACN ingest (LedFx-compatible).
- **P4 - fleet ops**: OTA over the P2c HTTP layer; per-node SoftAP
  secret + HMAC on whmcast; ESP-NOW/BLE pairing provisioning; EVENT
  packets (any-button-pauses-all class features).
- **P5 - audio remainder**: WAV, beep-vs-music ducking/mixing,
  visualization tiers, mic streaming.
- **P1 - media pipeline** (parallel track): .whm container + host
  converter, SD video/playlist player, gamma/power governor (art-gamma
  work now informs it), web UI riding the P2c server.

