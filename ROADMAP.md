# WHM roadmap

Status: v0.37.x - phase 0 long complete; the git era's first arcs
(walker sync doctrines 13-17, the art suite, the OTA suite) are
landed. Originals preserved in docs/.

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
