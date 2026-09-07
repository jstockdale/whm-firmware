# WHM roadmap

Status: Phase 0 COMPLETE at v0.31.2 (84 pre-git revisions + 2 commits).
The original phase-0 planning document is preserved at
docs/ROADMAP-p0-original.md.

## Done (phase 0 ledger, by subsystem)

Display and world
- HUB75 via LCD_CAM+GDMA, 150 Hz, double-buffered; 60 fps master grid
- Walker world: deterministic chunks, reward-table exploration, chair
  ritual, jump vocabulary, houses/ladders/cloud platforms
- Corrected parallax sky (sun/moon 0.04, stars 0.10, clouds 0.15 with
  leftward wind), time-true solar arc, dawn/dusk glow
- Shared-origin double-precision camera; walk speed dial (-2..2, 1x/s
  slew); cross-panel lockstep replicas with equal-step referee beacon
  (type-9), ownership handoff, dead-owner seizure
- Nightly midnight fireworks ritual; Dec 31 fleet-takeover NYE program
  (type-7): plaza scene, world freeze, ten-count, glyph constellations,
  friend walker + banner, generative per-year choreography
- Screens: clock, sensors, Life strip, music, Oracle, timer/stopwatch,
  overlays, Ignition boot flourish

Fleet
- WHM-LINK: election, TSF timebase, mDNS identity
- Deadline execution (type-2): one-timestamp rule, triple-send burst,
  lead dial (default 333 ms), late-runs execute
- Media sync by sha manifest; anchor self-target guard
- Wire types 1-9, all size-asserted

Audio
- Chime resurrected (close-before-open) with write-verdict forensics;
  vol/tone
- Mode A: master free-run + type-8 real-audio beacons; follower engine
  v3 rate-servo; name+sha track resolution; sorted scans; downbeat
  prints
- Mode B: DJ PCM streaming on :7778, WHMT framing, auto-tune, 2 s
  re-announce

Infrastructure
- NVS broker (internal-staged, success-only GET writes)
- Staged OTA + /fw self-serve; HTTP file service + manifest
- Console: in-house editor, armored help, hidden-command convention
- Git: repository live, doctrines in README, VERSION tradition
  continues

## Bench queue (built, awaiting hardware verdicts)

- 0.31.2 seam: sustained offset gone; jump/fall exactly at bezel;
  reverse crossings; serial-silence test (any drift line now carries
  step + delta)
- Mode A listen: master pristine full-song; follower ppm settle; walk
  the room for phase-hold; AP-burp immunity (master sails, follower
  confesses)
- Mode B premiere: dj from One, delete-a-track party trick, mid-song
  follower reboot re-tune, dj stop release
- Lead A/B at 500 (should now be zero added skew) then back to 333
- Broadcast media sync courteous form (anchor abstains)
- fw test (nightly arc) and fw nye fast (choreo review); real-rtc NYE
  takeover rehearsal (set one unit to Dec 31 23:54, watch it seize the
  fleet, tz label correct)
- Corrected night sky by eye; cloud direction; sorted mp3 list
  numbering match

## Next up - P1: controllable walker (deferred by choice)

Goal: pair a game controller and drive the walker around the levels,
pixel-perfect across panels under all reasonable network states, with
lockstep as the always-on fallback.

Controller reality check (captured 2026-09-07):
- ESP32-S3 is BLE-only - NO Bluetooth Classic. The DualSense (PS5)
  pairs over Classic, so it CANNOT connect to the S3 by radio.
- Compatible paths, pick at build time:
  a) BLE-native pads: Xbox Series X|S (BLE), Stadia (BLE, cheap,
     excellent), 8BitDo in BLE mode
  b) USB-host HID: S3 has OTG - a DualSense on a USB-C cable works,
     and wired input is the honest route to the 10 ms dream
  c) A classic-BT bridge (plain ESP32 forwarding HID over UDP) if the
     DualSense must be wireless

Latency law (John, 2026-09-07): scheduled input-to-motion must be
<= 25 ms; 10 ms is the target. Design consequences:
- Input replication (type-10): press/release events broadcast with
  execute-at-step; every replica applies at that step; sim stays
  deterministic INCLUDING the player. Horizon 1-2 steps (~16-33 ms)
  fits the 25 ms law on SoftAP or low-lead infra; UDP burst primary,
  TCP fanout mirror for reliability (sanctioned event channel - never
  the 60 Hz pose channel; TCP head-of-line stalls are why).
- Authority mode (the 10 ms path): while input is active, the unit
  holding the controller becomes temporary pose-authority streaming
  UDP-unicast pose; reverts to lockstep on idle. A mode inside the
  architecture, not a second architecture.
- Referee hardening: beacon gains rolling input-history CRC; a replica
  that missed an input detects and requests TCP replay - input loss
  self-heals instead of drifting.
- Input sources besides the pad: console (walker left|right|jump),
  BOOT button, HTTP endpoint (phone as controller).

## Backlog

Delights
- Walker easter eggs: summit flag, Meow the cat cameo, balloon,
  a "73" slab for the radio faithful
- Self-playing fleet Pong demo (fireworks demo shipped as the NYE
  program)
- Synthwave walker theme (sky/palette are parameterized enough now)
- Music easter egg hook at NYE midnight (scheduler hook exists)
- mp3 trim <ppm>: glacial master accuracy-trim (explicitly not
  convergence-chasing)

P3 - external control
- DDP and/or ArtNet ingest for the panels

P4 - hardening
- whmcast HMAC (types 1-9 are unauthenticated LAN-only by design
  today); signed OTA; /fw auth

P5 - audio expansion
- WAV support; ducking; visualizer; mic/line-in DJ input
- Mode B: variable sample rates (packet already carries rate);
  manual mp3 tune <host> command

Cosmetics and open investigations
- One uncomposed overlay blink at stage transitions (33 ms per 10 s)
- ES7210 mic-side i2s disable-E log noise (benign)
- sd ls errno instrumentation in place; LFN_HEAP suspicion unconfirmed
- v1 Mode B assumes 48 k stereo library

## Standing workflow

- Every change is a commit; releases are tagged; tarballs remain the
  flash artifacts; VERSION carries the release paragraph
- Release gate: zero diagnostics, version string, feature strings +
  symbols verified, artifact hashes match
- The twelve doctrines live in README.md
