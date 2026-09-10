# WHM roadmap

Status: v0.69.1 - phase 0 long complete. Originals in docs/.

## Media pipeline status (owner's question, answered in-doc)

- AUDIO computer->fleet: DONE and battle-tested. Load once (SD or
  a peer's /media), fleet fan-out bulk-fetch (bf_cli/bf_pump)
  pulls it everywhere; Mode A plays it TSF-locked across panels
  (rate-servo v3), Mode B DJ-streams live. Remaining items are
  bench VERDICTS (pristine-listen, premiere re-tests), not code.
- LIVE VIDEO computer->fleet: BUILT and first-lit (the
  signet/media arc). http://<panel>/live serves a browser capture
  page - screen share, camera, or a video file - downscaled to
  64 px and POSTed to /stream/frame; whm_media schedules each
  frame so it hits both panels at the same TSF instant. One
  browser visit re-verifies it on today's build.
- RECORDED video (.whm container on SD, playback sans computer):
  the arc's unfinished half - still open.
- P3 DDP/ArtNet ingest: backlog - the standards route for
  computer-driven pixels at scale.

## Flash headroom (owner's note, banked)

- The S3 module carries 32 MB; today's table uses two 3 MB OTA
  slots + 8 MB FAT with ~18 MB UNALLOCATED. When the app needs
  room (wolfSSL took us to ~59% of a slot), grow the slots via a
  custom partitions.csv - never diet the firmware first.

## The user's own sky (GPS constellations, future)

- Today: the worldwide four (Orion, Big Dipper, Cassiopeia,
  Cygnus) live in star-space, one per 512-cycle. Future:
  a user lat/lon (console-config; the LR1121 has no GNSS)
  plus dayclock time -> compute the ACTUAL visible
  constellations and place them - the fleet renders the
  owner's real night sky.

## Walker rituals (Robin's) - docs/walker-rituals.md

- THE CAMPFIRE: night-only by the DAYCLOCK; build (stone
  ring, wood, spark-to-flame), sit with glow halo +
  marshmallow beats, and the sacred ordering - DOUSE with
  the bucket (water arc, steam puff, dimming embers) before
  he ever leaves the scene. No new wire: states/phase carry
  it; fire anchors chair-style.
- THE HAMMOCK: day or night when the environment allows -
  anchors from the STRUCTURAL set only (ladders, trees,
  houses; never platforms), pairs 8-16 px apart, height
  wherever reachable (v1: one anchor a climbable ladder for
  elevated slings). Rope-throw, tie-offs, unfurl; sway /
  nap (floating z) / watch; roll-up takedown. Anchor
  discovery deterministic from chunk data - byte-free.
- Sequencing: after the monitor/Pro/LoRa arc; campfire
  first. Each ships fleet+monitor+viewer together.

## Monitor: multi-board + the LoRa gateway (planned)

- Second target: T-Display-S3 Pro (ST7796 SPI+DC, 49-col GRAM
  offset landmine banked, CST226SE touch, PWM backlight,
  SY6970) - docs/monitor-hw-pro.md is the spec. boards/
  abstraction + Kconfig MON_BOARD; per-board tarballs.
- LoRa telemetry gateway behind MON_HAS_LORA - HARDWARE
  CONFIRMED: owner's unit is the MVSRLora with an LR1121
  (915 MHz + 2.4 GHz LoRa; pins CS7/BUSY46/INT40/RST10 on
  shared SPI, DIO5/6 RF switch table banked verbatim, the
  camera port sacrificed for the radio). Plan: sub-32-byte type-32
  digests (pose + cam + rung + anchor/tsf echo) at 1-2 Hz -
  far monitors render the twin from digests; observers free
  at kilometer scale.

## Done 2026-09-08/09 - the convergence night (0.58.2 -> 0.62.0)

- Determinism trilogy: Pure Stream (stateless keyed rnd), Whole Pose
  (64B keyframes, six new fields), One Camera (pure f(anchor, step),
  rewind machinery deleted, type-10 belt resurrected via sizeof law)
- The court sighted: Near Horizon (K clamped inside the ring),
  materiality, retired watermark, soft-resync storm breaker, quiet
  replay + right clock + ordered epoch holding through live turns
- Gauges: the [W] ledger grew ok/or/snap/stale/wm/ms/pr/pn/tx/rx -
  and solved four roots in two captures
- The Oracle: type-16 state-hash ladder (strict core + soft tag),
  tools/whm_oracle_diff.py harness
- Fleet viewer v1-v3: UDP bridge + browser twin (verbatim world and
  sprite port), telemetry scope with reticle and panel legend,
  flight recorder with time-travel inspect and EXPORT
- docs/rsk-architecture.md: the library plan (laws, not code),
  extras adjudicated, milestones M1-M7
- Field: 10-minute verified run, then the overnight soak - synced by
  morning, pixel-perfect seam transits, multi-room observers

## Done since the import (by arc)

Walker synchronization (doctrines 13-15)
- Entry as forced replay point; step-time reward generator
- Shadow-sim keyframes at 3x sync-lead on unchanged-then-extended
  type-9 (52 B: future step, pose, tgt, vx)
- At-step verification (zero evidence age), complete-state snap,
  replay-resync as the sole realignment; snap-storm breaker
- TSF-ranked ownership claims; demotion grace; equal treatment of
  handoff and seizure; forensic snap dumps + transition trace;
  hidden walk pure bisection switch

Art suite
- Sun r11-15 living rim + orbiting ray; moon r9; three cloud
  species with dual running lights; flora 24-40 px at k=0.7 behind
  structures; V-beat gulls; synthwave pigeons; the synthwave hour;
  the comet registry (Encke first light Dec 2026)

Fleet & OTA
- Nightly ritual as fleet mini-takeover (type-7 year 0)
- fleet [@node] addressing hardened (case + .local forgiven);
  whm_sync_node_name(); sync prints own fw
- OTA: truncation law (partition-size truth), erase-before-connect
  (zero-window deadlock), status-screen takeover (quiesces load by
  construction), self-source guard, server abort logging
- First successful unit-to-unit OTA; first watched update

## Done 2026-09-10 - rituals, anchors, SSH, RAM (0.63.0 -> 0.69.1)

- Robin's rituals x3 surfaces: THE CAMPFIRE v2 (seat +5, 4-stone
  ring, three-log build, tall flame + licks, smoke on the wind,
  ember riders, douse law kept) and THE HAMMOCK v1 (structural
  anchors, distance-zero pick, sway/nap/watch).
- The sky at 3x: the worldwide four as ANCHOR features (Orion 21
  tall, Dipper 27 wide) with 4-neighbour halos; monitor Pro board
  DONE (multi-board half of that section complete; LoRa half
  planned as written).
- SSH landed (whole arc): wolfSSH server, pubkey-only, per-device
  NVS host key fail-closed, 128K PSRAM crypto arena (all four
  placement conditions), THE FLEET BRIDGE (per-task fopencookie
  stdout + esp_console_run = every fleet verb over SSH verbatim),
  live ESP_LOG tee, panel MOTD, SD-primary keystore
  (/sdcard/.ssh/authorized_keys) with NVS fallback. Field lessons
  banked: mbox v1/v2 (construction beats probing), the chime
  conviction (log hooks pack nothing they cannot carry in
  registers), the SD gaslight.
- RAM audit wave 1 (docs/RAM-AUDIT-2026-09-10.md): statics already
  lean; five stacks to PSRAM (~23K); wave-2 banked. Field ladder:
  three clean boots -> ble enable -> mem tasks.
- fw grammar complete: fw test|nye [fast], uniform.

## Bench queue

- Matched-fw walker soak: walk stats verdict (ok climbing, snap ~0);
  seam jumps; reverse crossings; 10-min anchor rollover as non-event
- First full-theater OTA watch (ERASING -> ... -> REBOOTING)
- Mode A pristine-master listen + ppm settle + AP-burp immunity;
  Mode B premiere (delete-a-track, mid-song reboot re-tune)
- lead 500 zero-skew A/B; broadcast media sync courteous form
- fw test fleet-nightly premiere; fw nye fast review; real-rtc NYE
  takeover rehearsal
- Art tour by rtc: day / dusk / night / 03:30 synthwave; comet
  preview at 2027-01-25 evening; tree-parallax 0.7 verdict

## P1: controllable walker - IN FLIGHT (P1.1 landed v0.44.0)

P1.1, the input spine, is live: type-11 events applying at their
stamped step on every replica; a step-stamped log consumed by
wk_step itself so live stepping and replay are ONE consumer (late
or missed events self-heal via snap-storm -> replay); USER mode
suspends autonomy with a 300-step shake-off; jump = the charged
long-jump; console verbs walk left|right|stop|jump|auto; the
tunnel carries type 11 so watch viewers stay lockstep. Timing:
next-step-boundary-bound - 0..33 ms, mean 16.5 - meets <=25 in
expectation, worst one tick, stated honestly. The wrist drives it
TODAY through the L2 bridge. SEEK (act 5, walk to <x>, v0.44.1) makes the
watch-visit steerable today. Remaining (P1.2+): BOOT-button verbs,
BLE pads / USB-host HID / classic bridge, phone-HTTP, local
cosmetic ack at input instant, owner short-lead ramp.


Latency law: <=25 ms scheduled input-to-motion; 10 ms target.
Down-payments already landed: the future-ref buffer IS the
input-invalidation substrate (flush s_wkf on input, owner re-emits
short-lead); authority-mode escape hatch = the ownership machinery.

Controller reality: ESP32-S3 is BLE-only; DualSense pairs Classic.
Paths: (a) BLE pads (Xbox Series, Stadia, 8BitDo BLE), (b) USB-host
HID - wired DualSense, the honest 10 ms path, (c) classic-ESP32
bridge. Type-11 input events (type 10 = WK_PARAMS, shipped v0.43.0), exec-at-step, UDP burst + TCP mirror
(events only - never the pose channel). Sources: pad, console
(walker left|right|jump), BOOT button, phone-over-HTTP.

## P1.5: wh-link BLE control channel (designed - docs/WH-LINK-ADAPT.md)

wh-link v1 adopted verbatim (conformance suite passes on the build
host); panels join the Whitehat piconet as NimBLE peripherals on
the ring's proven GATT contract. BLE = control/pairing/status only
- never data or timing; acceptance = walker soak stays snap~=0
with a session active. CONSOLE 0x2F tunnels the existing console
grammar (sealed-only); STATUS 0x62 puts the fleet indicator on a
wrist; SAS pairing renders on the panel with BOOT-button confirm.
v2 (owner direction): full piconet CITIZENSHIP - tiered timing
law (Tier 0 us/TSF untouched; Tier 1 ms via wh-link TIMESYNC,
connection-anchor refinable; Tier 2 bulk), WH_MSG_WHMCAST tunnel
(verbatim whmcast over BLE - replicas transport-blind), hybrid
bridge for WiFi-orphan panels, BLE-only fleet-in-a-field mode
(everything but Mode A), and panels as the piconet's display
surface (DEVICE_SEEN ticker, THREAT_EVENT flash, HID_PROXY into
P1 input). Phases L0-L8; upstream items: WH_ROLE_PANEL=4 +
WH_MSG_WHMCAST.

## Scope guard (owner directive)

Core before creep: P1 controllable walker (the <=25 ms law), the
Mode A/B listening verdicts, the walker soak verdict, and the NYE
rehearsal FINISH before new panel display features start. The
watch-integration SPECS ship now (docs/SPEC-panel-link.md,
docs/SPEC-walker-port.md, docs/TO-WATCH-AGENT.md) because specs
unblock the other project in parallel at document cost; the
piconet-stats/graphs display surface is explicitly GATED behind P1.
Watch ranking received (TO-PANEL-AGENT) = the build order: 1 threat
interrupt (0x61 reuse), 2 find ticker (0x60 reuse), 3 PANEL_STAT
rate graph (0x6A), 4 GPS/time, 5 battery glyph, 6 LoRa.

## Operator visit (INVERTED per owner - TO-WATCH-AGENT-4; panel side DONE)

The owner's inversion deletes the coex problem: panels BEACON
(the L4 pseudonyms, already ~1 Hz), the watch SCANS (its day
job). Per-bond keys give the watch panel IDENTITY for free; the
connected lead is ranged via session RSSI. OPKEY 0x6B withdrawn;
canonical batch back to four; the Tier-0 sniffing flag deleted
rather than gated. Election + policy live watch-side; actuation =
existing CONSOLE / SEEK. Panel implementation cost: zero lines -
everything shipped v0.41.0-v0.44.1. Type-12 reserved-optional.

## Backlog

Delights: walker easter eggs (summit flag, Meow cameo, balloon, 73
slab); self-playing fleet Pong; NYE music-egg hook; mp3 trim <ppm>
P3: DDP / ArtNet ingest
P4: whmcast HMAC; signed OTA; /fw auth
P5: WAV; ducking; visualizer; mic/line-in DJ; Mode B variable
rates; manual mp3 tune
Sync: unicast referee/keyframe transport (300 ms -> 5 ms detection)
Cosmetics & open: overlay blink 33 ms/10 s; ES7210 disable-E noise
(benign); sd ls LFN_HEAP suspicion unconfirmed; v1 Mode B assumes
48 k stereo

## Standing workflow

Every change is a commit; releases are tagged; tarballs remain the
flash artifacts; the release gate holds (zero diags, version string,
suffix-safe feature strings, symbol + hash verification). The
seventeen doctrines live in README.md.
