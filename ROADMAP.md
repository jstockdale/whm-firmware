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

## Next up - P1: controllable walker (deferred by choice)

Latency law: <=25 ms scheduled input-to-motion; 10 ms target.
Down-payments already landed: the future-ref buffer IS the
input-invalidation substrate (flush s_wkf on input, owner re-emits
short-lead); authority-mode escape hatch = the ownership machinery.

Controller reality: ESP32-S3 is BLE-only; DualSense pairs Classic.
Paths: (a) BLE pads (Xbox Series, Stadia, 8BitDo BLE), (b) USB-host
HID - wired DualSense, the honest 10 ms path, (c) classic-ESP32
bridge. Type-10 input events, exec-at-step, UDP burst + TCP mirror
(events only - never the pose channel). Sources: pad, console
(walker left|right|jump), BOOT button, phone-over-HTTP.

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
