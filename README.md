# WHM - synchronized LED matrix firmware

Firmware for a small fleet of 64x64 HUB75 LED matrix units built on the
Waveshare ESP32-S3-RGB-Matrix board. Panels elect an anchor, share a
microsecond timebase over WiFi TSF, execute commands against fleet-wide
deadlines, sync media libraries by sha, play music in two synchronized
modes, and render a continuous side-scrolling world in which a small
walker lives, keeps a nightly midnight ritual, and throws a New Year's
party that takes over every screen.

Phase 0 (bring-up) - imported at v0.31.1 after 83 numbered pre-git
release tarballs; living history since, one commit per change. The
VERSION file tradition (one paragraph of release notes per build)
continues in-repo. This README describes the tree as of v0.37.x.

## Hardware

- Board: Waveshare ESP32-S3-RGB-Matrix (ESP32-S3-N32R16: 32 MB octal
  flash, 16 MB octal PSRAM)
- Panel: 64x64 P2.5 HUB75E, 1/32 scan, FM6126A shift drivers, driven
  via LCD_CAM + GDMA descriptor chains at 20 MHz (~150 Hz refresh,
  double-buffered, 8-bit BCM)
- Peripherals per unit: SDMMC microSD, QMI8658 IMU, SHTC3 temp/RH,
  PCF85063 RTC, ES8311 speaker DAC + ES7210 mic ADC, BOOT button
- Bench fleet: units named One, Two, Three; home AP is ordinary infra
  WiFi (DTIM-3 timing is a first-class design constraint, see
  Doctrines)

## Build

ESP-IDF v5.5.2.

    source $IDF_PATH/export.sh
    idf.py build
    idf.py -p /dev/ttyACM0 flash monitor

sdkconfig is committed deliberately: the release gate depends on exact
configuration (PSRAM mode, console, partition table). Release
philosophy: a build ships only when diagnostics are zero, the version
string is present in the binary, feature strings and ELF symbols for
the release's claims are verified, and the packaged artifact hashes
match the build.

## Subsystems

### Fleet (WHM-LINK)

- mDNS identity (<name>.local), UDP whmcast on :7777, anchor election
  by priority, roles auto/anchor/follow/conductor
- Shared clock: WiFi TSF, locked once associated; `tsf` shows drift
- Deadline execution: `fleet <console line>` stamps one exec_at
  consumed by both the packet and the sender's own deferral (one
  timestamp, two consumers). Per-rig lead dial `sync lead <ms>`
  (100-1000, NVS, default 333 - clears DTIM-3 worst case ~307 ms).
  Triple-send bursts at 0/60/120 ms; the dedupe ring makes repeats
  free. Late lines still execute: convergence beats punctuality.
- Media sync: `sync media <host>` pulls by manifest with sha64
  verification; anchors politely refuse to pull from themselves.
- `sync status` prints rx/drop counters, lead, and role.

### Wire protocol (whmcast, ver 2)

| type | size | purpose |
|-----:|-----:|---------|
| 1 | ann    | anchor announce / election |
| 2 | 212 B  | deadline command (exec_at, target[16] addressing, burst x3) |
| 3 |        | Life strip world sync |
| 4 |        | Mode A play (name + sha + start_tsf) |
| 5 |        | (reserved) |
| 6 | 96 B   | Mode B DJ announce (rate, title, from) |
| 7 | 44 B   | NYE fleet takeover (tz label, year, start_tsf) |
| 8 | 44 B   | Mode A master beacon (tsf, content idx) |
| 9 | 52 B   | walker keyframe / ownership (seq = FUTURE step; pose + tgt + vx; claims ranked by tsf) |

All structs are packed with _Static_assert on wire size.

### Audio

- `vol` (system), `chime`, `tone`; the boot chime plays from the
  Ignition flourish and reports per-chunk write verdicts
  (chime: done (59 chunks) or a named first error).
- Mode A - synchronized file playback (`mp3 fleet <n>`): the issuing
  unit is master and free-runs (no servo, no resampler - pristine
  passthrough), publishing type-8 beacons of its real audio position
  at 2 Hz. Followers run engine v3: a rate-servo linear resampler
  (delta clamped +-150 ppm, slewed) locking to the master's beacons -
  inter-unit phase is what ears hear. Tracks resolve by name and sha,
  never index; scans are sorted so numbering matches across cards.
  The music screen shows drift and servo ppm (S+0.2 -8p), or
  M free-run on the master.
- Mode B - DJ streaming (`mp3 dj [n|stop]`): the anchor tees decoded
  PCM into a ring served by a select-loop TCP server on :7778 (up to
  3 clients, 100 ms WHMT marker framing). Followers auto-tune on
  type-6, buffer 150 ms in PSRAM, and reuse engine v3 unchanged via a
  pluggable due-function. Late joiners catch the 2 s re-announce.

### Walker world

A deterministic, endless side-scroller shared by every panel:

- World is pure f(world-x): chunk platforms, houses with night-lit
  windows, ladders, cloud and treetop platforms - identical on every
  unit by construction.
- Camera: one blessed wk_cam_tick per frame advances a
  double-precision accumulator seeded from the shared TSF clock -
  shared origin (seams match regardless of boot order) and 1e-10
  quantum (no float freeze). wk_cam() is a pure reader. `walk speed
  <-2..2>` slews at 1x/s; `fleet walk speed` keeps panels lock-step.
- Cross-panel walker (doctrines 13-15): every unit runs the same
  TSF-anchored fixed-timestep replica with pure f(anchor, step,
  draw#) randomness - motion needs no packet at all. Entering the
  pattern is a forced REPLAY POINT (respawn + catch-up from the
  shared anchor), so resuming and fresh units rebuild identically.
  The owner's SHADOW SIM runs 3x sync-lead into the future and emits
  KEYFRAMES on type-9 (seq carries the future step; complete state
  including tgt and vx). Replicas verify AT-STEP with zero evidence
  age: match consumes silently, mismatch is a complete-state SNAP at
  exactly the described step - never a nudge (a nudged deterministic
  replica diverges at walking speed forever). A snap storm escalates
  to REPLAY-RESYNC, the provably-exact rebuild. Ownership follows
  containment; claims rank by beacon TSF; demotion grants the same
  600 ms beacon grace as handoff, so mutual silence cannot form; a
  silent owner is seized after 1.2 s. `walk` prints refs
  ok/snap/stale; hidden `walk pure on|off` strips reward influence
  for bisection.
- Parallax sky: screen = wx - (k*cam + 64*idx); k on camera only so
  seams stay continuous. Sun/moon k=0.04 (time-true solar arc, dusk
  swell), stars 0.10 (twinkle), clouds 0.15 plus a gentle 0.25 px/s
  leftward wind.
- Rituals: reward-table exploration (marks are pure chunk-entry
  events inside the step - doctrine 13), the camping-chair break,
  bezel shimmy at cluster edges, and fireworks (below).

### Sky, art, and visitors

- Sun r11 (r15 at dusk) with a LIVING RIM - a hot-spot rotating
  ~8 s/rev over a breathing glow - plus a two-pixel ORBITING RAY.
  Moon r9, same treatment dimmer, traced around the crescent.
- Three cloud species (wisp, double puff, big cumulus with shaded
  base), each bobbing on its own phase, wearing TWO running lights:
  a bright 3 px spot along the top edge and a dimmer 5-6 px
  counter-current along the bottom.
- Flora at parallax k=0.7 on the shared k*cam + 64*idx form, drawn
  BEFORE platforms so the walker walks through trees: large oaks
  (28-34 px, ragged three-shade canopies) and layered pines
  (26-38 px) on ~30%% of chunks, mediums on 25%%, the old tiny kinds
  as dimmed distant accents. Night-dimmed; silhouetted in synthwave.
- Fauna: seagulls by day - a two-frame V-BEAT (shallow-V glide,
  deep-V flap; one vertex is a bird) at star parallax - and a rare
  magenta-underlit synthwave pigeon in the small hours.
- THE SYNTHWAVE HOUR: 3-5 am local with 15-minute fades - purple to
  hot pink, cyan/magenta stars, the big sun in horizontal
  skip-lines. Keys to each panel's local clock, like the day/night
  sky: across a multi-timezone fleet the seam is a timezone
  boundary, which is a feature.
- THE COMET REGISTRY: when a notable comet is genuinely in Earth's
  sky, one appears here - two-tail pixel anatomy (shimmering dust,
  flickering cyan ion), intensity ramping across the real
  visibility window, name nod in gold 3x5 glyphs for the first 12 s
  of every 10th minute. Registry as researched 2026-09-07: 2P/Encke
  (Dec 2026 - Feb 2027), C/2026 C1 Tsuchinshan (Oct-Dec 2028),
  46P/Wirtanen (Sep-Dec 2029), 103P/Hartley 2 (Mar-May 2030). Dates
  are astronomy; brightness is weather - the registry updates by
  commit + fleet OTA when the sky changes.

### Fireworks and New Year

- Nightly: a fleet mini-takeover (type-7 with year 0) - one
  initiator at its local 23:59:40 broadcasts a TSF-locked 25 s
  script every replica runs identically: chair, ten seconds of
  seeded fireworks, linger, release. The whole cluster celebrates
  together (local-clock st-forcing hard-forked replicas across
  timezones; the fleet script is its own determinism).
- Dec 31: a 20-minute fleet program. The unit whose local clock hits
  23:55 becomes initiator and takes every panel via type-7 (TSF-locked
  lockstep; gold tz label names whose midnight). A plaza scene scrolls
  in and the world freezes at 23:58; ember-digit ten-count; a
  ten-minute show opening with glyph-burst HAPPY / NEW YEAR and the
  computed year as twinkling constellations; at 00:10 a friend walker
  (teal scarf) helps hoist the HAPPY <year> banner and exits stage
  left; 00:12-00:15 the world eases back and the party scrolls into
  memory. Choreography is PRNG-seeded by the year - different forever.
- Hidden test command: `fw test` (nightly arc now), `fw nye [fast]`
  (full program, or 10x for review), bare `fw` disarms.

### Screens and patterns

Clock (labels, tz), sensors, Life (fleet-continuous rainbow strip),
music (art / info layouts, drift line), Oracle (Ignition-language
eight ball), timer/stopwatch, text overlays (text --over), span text,
wanderer sprite, and the Ignition boot flourish (chime-locked rings,
name dissolve, scarf underline).

### Infrastructure

- NVS broker: all settings RPC through an internal-stacked task;
  values stage through internal temporaries both directions, GETs
  write outputs only on success (caller defaults survive missing
  keys). PSRAM-stacked tasks are flash-safe by construction.
- OTA: `ota <host|url>` pulls the source's full partition image
  (both units share the partition table, so the expected size is
  known locally - no header trusted; short reads are named
  TRUNCATED, never validated). The slot ERASE runs BEFORE the
  connection opens (a 10-20 s erase after open() starved the server
  into its 5 s send timeout - the zero-window deadlock). The
  receiving panel becomes an OTA STATUS SCREEN: version arrow,
  progress bar with the running-light sweep, phases ERASING ->
  PULL -> VERIFYING -> REBOOTING, or red FAILED + reason, restoring
  the previous screen after 6 s. Every unit serves its running
  image at /fw (X-WHM-FW version header; server aborts logged with
  offset and cause). `ota` against your own name abstains ("I'm the
  source"), so `fleet ota <source>` is safe fleet-wide.
- HTTP :80 - file service, /manifest (sha64), /media/<f>, /fw.
- Console: in-house line editor (history, ctrl-c), armored help
  (NULL-safe, empty-syntax rows are hidden), hidden commands `fw` and
  the `walk` family documented only here.

## Console quick reference

Run `help` on-device for the full list. Notables:

    sync lead <ms>        fleet deadline budget (default 333)
    fleet [@node] <line>  run fleet-wide, or on one node (@Two)
    ota <host> | status   pull an update / show slots
    mp3 fleet <n>         Mode A synchronized playback (master = you)
    mp3 dj [n|stop]       Mode B DJ stream from this unit
    walk [speed <-2..2>]  world scroll: 0 pause, negative reverse
    fw test|nye [fast]    fireworks test (hidden)
    vol [1-100]           chime/beep/tone volume

## Doctrines

Hard-won laws, recorded so they never have to be re-learned:

1. Shared origin is a fleet invariant. Any per-unit rebase of shared
   state (the camera) breaks seams and twins the walker. Precision
   problems are solved with wider types, not new origins.
2. Purity is load-bearing. Converting a pure function to stateful is
   an interface change even when the signature does not move - the
   callers' habits are part of the contract.
3. One timestamp, two consumers. Sender and receivers must consume
   the same stamped deadline; parallel computations of "the same"
   time will diverge (the forgotten +150 ms).
4. TSF magnitude breaks float accumulation. t/1e6 lives near 1e6
   where float's quantum is 0.0625; integrate in double.
5. PSRAM stacks are for tasks that never touch flash. Anything
   running console lines or NVS gets an internal stack; the NVS
   broker exists so everyone else can be PSRAM.
6. Broker GETs write outputs only on success. Clobbering caller
   defaults with zeros turns missing keys into black panels.
7. Undocumented means empty string, never NULL. help printed NULL
   once; the printer is armored now, but the rule stands.
8. Deterministic replicas beat follow-the-leader at a seam. 10 Hz +
   DTIM is pixels of lag; lockstep is zero. Keep a referee beacon
   with a dead-band so real drift confesses in pixels.
9. Stateful RNG breaks lockstep. Replica randomness must be pure
   f(anchor, step, draw#), independent of draw history.
10. Fixed budgets lose to dials. Radio timing (DTIM) is per-rig;
    expose the lead, default it to the measured worst case.
11. Comments are not gate strings. A release gate greps the binary;
    only runtime strings land in .rodata.
12. Twin functions eat blind patches. beep and chime share a tail;
    edits target by function span, verified per-tag, never by
    replace-first.
13. A shared generator must be shared in step-time, not wall-time.
    Wall-clock inputs make replay != continuous.
14. Never mutate a deterministic replica. A nudged replica meets
    every threshold at offset positions and diverges at walking
    speed forever. Realign only by replay, or by complete-state
    snap at exactly the step the state describes.
15. Tell the future before it happens. A deterministic owner can
    emit keyframes AHEAD of need (shadow sim, 3x the delivery
    worst case); replicas verify at-step with zero evidence age.
    Trailing telemetry is always a chase; leading telemetry is a
    contract. User input invalidates the future - and that IS the
    input architecture. Complete state means COMPLETE: a snap that
    omits any sim field (tgt, vx) leaves the replica chasing its
    own target.
16. The linker eats suffixes. A release gate that greps -x for a
    short literal will false-negative when it tail-merges into a
    longer one (FAILED into ESP_ERR_*_FAILED). Gate on strings that
    cannot be suffixes.
17. Read-before-write applies to designs, not just files. The fleet
    had [@node] addressing all along - wire field, rx filter, help
    entry - and it was nearly duplicated from stale memory. Grep
    for the feature before building the feature.

The original stage-by-stage validation matrix and the whm_board.h
provenance notes live in docs/BRINGUP.md.

## History

Everything before the first commit lived as 83 numbered release
tarballs, a growing doctrine list, and one memorable lost hour that
had to be recovered by diffing the project against its own delivered
artifacts. This repository exists so that never happens again. The git era
delivered: the walker-sync campaign (doctrines 13-15), the art
suite, the comet registry, and - after the zero-window deadlock
fell - the first successful unit-to-unit OTA in the project's
history, followed shortly by the first one ever watched on the
panel it was updating.

## License

To be chosen by the author. (Nothing in-tree assumes one yet.)
