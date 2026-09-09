# WHM — the Whitehat Matrix fleet

Synchronized 64x64 HUB75 LED panels that share one world, one clock, and one
song. Each node is a Waveshare ESP32-S3-Matrix (ESP32-S3, 16MB PSRAM, SD,
ES8311/ES7210 audio, IMU/RH/RTC) driving an FM6126A panel at 26.67MHz,
`lsbMsbTransitionBit=0`, 7-bit CIE1931 - full-weight bit planes at ~200Hz.

A tiny pixel walker lives across the fleet: he crosses bezels mid-stride,
leaps between platforms when physics allows, sits to watch the sun, and -
one day in four, in the golden hour - walks past the Golden Gate, the Taj,
or Stonehenge, depending on where his panel believes it lives.

## Feature matrix

| Area | What ships |
|---|---|
| World | Deterministic walker sim (pure f(anchor, step)); flora, birds-in-formation, sun v3, World Wonders; platform hops + style hop-offs |
| Sync | WiFi TSF shared clock (+-us); elections (prio, higher-name tiebreak); type-9 whole-pose keyframes 30Hz; One Camera - pure f(anchor, step), advanced only in the step loop, type-10 belt snaps residuals >1.5px |
| Ownership | The Baton (grace-until-successor-heard), the Edge Law (closest-panel off-world), the Cold Open (deterministic strip-0 crown at every respawn) |
| Convergence | The promise court: clairvoyant keyframes judged at equal step (Near Horizon K<=ring-2); materiality (position/state only earns a tape); adopt-always caps forks at one step; snap storms end in SOFT resync - the epoch is fleet property |
| Oracle | Type-16 state-hash rungs every 32 steps: canonical-manifest FNV64 strict core + soft tag over seen/visits; divergences confess with step, both hashes and the full manifest |
| Telemetry | 1Hz `[W]` ledger per unit; UTC-stamped SNAP tapes and ORACLE confessions; passive browser fleet viewer (tools/) - any number of silent observers ride the broadcast for free |
| Audio | Mode-A: fleet playback sample-locked via rate-servo (+-150ppm, ~250us) to the master's real DAC via type-8 beacons; Mode-B DJ tee :7778; 1Hz `[A]` ledger |
| Security | Ed25519 identity + TOFU pins (type-13); signed command envelope (284B, replay-windowed); X25519 fleet keywrap (type-14); keyed-BLAKE2b tags on all state (the Seal); `secure strict` |
| Ops | OTA (self-judging), HTTP media replicate + sidecar hash cache, console over USB-JTAG, status/keys/secure/audio diagnostics |

## Quickstart

Prereqs: ESP-IDF **v5.5.2** exported.

    idf.py build            # release gate: zero diags + feature-string
                            # probes + components/whlink/verify.sh
    idf.py -p /dev/ttyACM0 flash monitor

Or flash a release tarball (bootloader + partition table + app included):

    tar xzf whm-bringup-p0_<ver>-flash.tar.gz && cd whm-bringup-flash
    # see FLASHING.md inside; then:
    python3 -m serial.tools.miniterm /dev/ttyACM0 115200

First boot: the panel mints an Ed25519+X25519 identity, joins WiFi, locks
TSF, TOFU-pins its peers (~15s), and the anchor mints + wraps the fleet key.

## Console (the greatest hits)

    status                  # sync, ident (fp/pins/seal), audio servo, heap
    sync auto|anchor|prio   # elections; 'sync media <host>' replicates SD
    pattern walker          # the world (Cold Open decree on every entry)
    walk to <x> | walk stop # summon him; the Edge Law owns off-world
    walk diag [off]         # the 1Hz [W] ledger (see below)
    hud on|off              # strip tags + top-right clock (persisted)
    mp3 list|play|fleet     # 'mp3 fleet play <n>' = the Mode-A premiere
    mp3 diag off            # silence the 1Hz [A] ledger
    keys [forget <name>]    # identity + TOFU pin management
    secure [strict on|off]  # the Seal: state-plane enforcement
    screen next|auto        # music/oracle/home; oracle shake

## The [W] ledger (1Hz while the pattern runs; `walk diag off` to hush)

    [W] step= lag= st= own=N[/ME] sx= cam= kf ok= or= snap= stale=
        wm= ms= pr= pn= tx= rx= [ REPLAY]

| Field | Meaning |
|---|---|
| step / lag | window step; want-steps (visible during replay) |
| st / own | 4-char state; owning strip, `/ME` if this unit |
| sx / cam | walker x minus cam (fleet-screen x); One Camera accumulator |
| ok / or | promise verdicts matched; oracle rungs matched (0.62.0+) |
| snap | MATERIAL corrections (position/state); cosmetics heal silently |
| stale | frames at/behind current step - latency cosmetic, ~50% of rx is normal |
| wm / ms | cross-epoch corridor strays; promises expired unverified |
| pr / pn | valid future promises banked; nearest promise delta (+1..+6 healthy, -1 none) |
| tx / rx | wire tallies (rx is ground truth for peer health) |

Healthy non-owner: ok +25-30/s, pn small-positive, snap resting, wm/ms ~0.
Any frozen gauge now arrives with its cause printed beside it.

## Fleet viewer (tools/)

    python3 tools/whm_bridge.py        # UDP :7777 -> ws://host:8777
    # open tools/whm_viewer.html, connect - terrain twin, telemetry
    # scope with reticle, flight recorder with click-to-inspect + EXPORT
    python3 tools/whm_oracle_diff.py one.log two.log   # first-divergence finder

Observers are free: the viewer never transmits - kitchen, bedroom, N rooms,
same broadcast. Full world+sprite ported verbatim from ui.c (pure f(chunk));
golden-hash lock lands with RSK M2.

## Wire protocol (UDP :7777 broadcast, magic WHML)

| Type | Size | Purpose |
|---|---|---|
| 1 | ann | announce/election + telemetry |
| 3 | 212 / **284 signed** | fleet command (deadline-exec; Ed25519 + nonce) |
| 8 | +16 sealed | Mode-A master beacon (tsf, sample idx) |
| 9 | +16 sealed | walker keyframe (30Hz, owner truth) |
| 10 | +16 sealed | wkparams (anchor, cam position, scroll Q8) |
| 11 | +16 sealed | walker input |
| 13 | 88 | IDENT (name + ed25519 + x25519 pks; TOFU) |
| 14 | 112 | KEYWRAP (fleet key, per-peer X25519 AEAD) |
| 16 | 24 / 40 sealed | oracle rung (anchor byte, step, core h64, soft h16); rx gates obey sizeof - the only law |

Sealed = 16-byte keyed-BLAKE2b tag; unsigned rides a grace warning until
`secure strict on`.

## Repository

    main/            first-party firmware (SPDX GPL-3.0-or-later)
    main/monocypher.*, main/minimp3.h   vendored (see THIRD_PARTY.md)
    components/whlink/                  BLE side-channel (verify.sh gate)
    docs/            SECURITY.md, rsk-architecture.md (library plan),
                     audits, watch-agent letters
    tools/           whm_bridge.py, whm_viewer.html, whm_oracle_diff.py
    dist-flash/      release-shaped flash set (VERSION, FLASHING.md)

## Field record

0.61.1 soaked overnight unattended: pixel-perfect seam transits by
morning, dozens of live epoch turns, two passive browser observers in
other rooms. Divergences still occur - by design they now have a
ceiling and a half-life: detected within steps, converged within one,
storms ending in soft resync. Wrongness decays faster than it
accumulates; that inequality held for eight hours.

## Known gaps (the honest ledger)

- Oracle rungs need 0.62.0 on both units; `rung -` in the viewer means
  type-16 was never heard, not that anything is wrong.
- `stale` ~50% of rx is latency-vs-K cosmetics (tunable, harmless).
- `wk_visits`/seen ring is soft-unsynced (oracle soft tag watches it).
- Viewer cam uses type-10 snaps + slope EMA (Q8 ramp scale unverified
  client-side; sub-0.7px between snaps). NYE plaza + fireworks are not
  simulated in the twin. One +-1px stride nuance pends the M2 golden lock.
- The old fork-origin class is caged at <=1 step / ~0.3px by
  adopt-always; its next material SNAP tape names the birth step.

## License

GPL-3.0-or-later for all first-party code (see `LICENSE`); complete
third-party inventory and the BSD-3-relicense audit in `THIRD_PARTY.md`.
Spoiler: nothing in the link graph is copyleft but us.

73
