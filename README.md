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
| Sync | WiFi TSF shared clock (+-us); elections (prio, higher-name tiebreak); type-9 keyframes 30Hz; closed-form twin cameras (bidirectional rewind-exact) |
| Ownership | The Baton (grace-until-successor-heard), the Edge Law (closest-panel off-world), the Cold Open (deterministic strip-0 crown at every respawn) |
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
    mp3 list|play|fleet     # 'mp3 fleet play <n>' = the Mode-A premiere
    mp3 diag off            # silence the 1Hz [A] ledger
    keys [forget <name>]    # identity + TOFU pin management
    secure [strict on|off]  # the Seal: state-plane enforcement
    screen next|auto        # music/oracle/home; oracle shake

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

Sealed = 16-byte keyed-BLAKE2b tag; unsigned rides a grace warning until
`secure strict on`.

## Repository

    main/            first-party firmware (SPDX GPL-3.0-or-later)
    main/monocypher.*, main/minimp3.h   vendored (see THIRD_PARTY.md)
    components/whlink/                  BLE side-channel (verify.sh gate)
    docs/            SECURITY.md, ROADMAP, audits, watch-agent letters
    dist-flash/      release-shaped flash set (VERSION, FLASHING.md)

## License

GPL-3.0-or-later for all first-party code (see `LICENSE`); complete
third-party inventory and the BSD-3-relicense audit in `THIRD_PARTY.md`.
Spoiler: nothing in the link graph is copyleft but us.

73
