# WHM - synchronized LED matrix firmware

Firmware for a small fleet of 64x64 HUB75 LED matrix units built on the
Waveshare ESP32-S3-RGB-Matrix board. Panels elect an anchor, share a
microsecond timebase over WiFi TSF, execute commands against fleet-wide
deadlines, sync media libraries by sha, play music in two synchronized
modes, and render a continuous side-scrolling world in which a small
walker lives, keeps a nightly midnight ritual, and throws a New Year's
party that takes over every screen.

Phase 0 (bring-up) - imported at v0.31.1 after 83 numbered pre-git
release tarballs. The VERSION file tradition (one paragraph of release
notes per build) continues in-repo.

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
| 2 | 212 B  | deadline command (exec_at, burst x3) |
| 3 |        | Life strip world sync |
| 4 |        | Mode A play (name + sha + start_tsf) |
| 5 |        | (reserved) |
| 6 | 96 B   | Mode B DJ announce (rate, title, from) |
| 7 | 44 B   | NYE fleet takeover (tz label, year, start_tsf) |
| 8 | 44 B   | Mode A master beacon (tsf, content idx) |
| 9 | 44 B   | walker pose / ownership beacon |

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
- Cross-panel walker: lockstep replicas - every unit simulates the
  same TSF-anchored fixed-timestep machine with a pure
  f(anchor, step, draw#) RNG, so positions are bit-identical with no
  communication. The type-9 ownership beacon acts as referee: 1.5 px
  dead-band, beyond it adopt-and-confess (walker: drift Npx
  corrected). Ownership follows containment in the moving strip
  windows; handoff prints on both sides; a silent owner is seized
  after 1.2 s.
- Parallax sky: screen = wx - (k*cam + 64*idx); k on camera only so
  seams stay continuous. Sun/moon k=0.04 (time-true solar arc, dusk
  swell), stars 0.10 (twinkle), clouds 0.15 plus a gentle 0.25 px/s
  leftward wind.
- Rituals: reward-table exploration, the camping-chair break, bezel
  shimmy at cluster edges, and fireworks (below).

### Fireworks and New Year

- Nightly: at 23:59:48 local the walker stages his chair; ten seconds
  of seeded fireworks at midnight; he lingers, packs, and moves on.
  Each unit honors its own timezone.
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
- OTA: `ota <host|url>` stages and validates; every unit serves its
  running firmware at /fw.
- HTTP :80 - file service, /manifest (sha64), /media/<f>, /fw.
- Console: in-house line editor (history, ctrl-c), armored help
  (NULL-safe, empty-syntax rows are hidden), hidden commands `fw` and
  the `walk` family documented only here.

## Console quick reference

Run `help` on-device for the full list. Notables:

    sync lead <ms>        fleet deadline budget (default 333)
    fleet <line>          run a line fleet-wide at one deadline
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

The original stage-by-stage validation matrix and the whm_board.h
provenance notes live in docs/BRINGUP.md.

## History

Everything before the first commit lived as 83 numbered release
tarballs, a growing doctrine list, and one memorable lost hour that
had to be recovered by diffing the project against its own delivered
artifacts. This repository exists so that never happens again.

## License

To be chosen by the author. (Nothing in-tree assumes one yet.)
