# wh-link on WHM — BLE control-channel adaptation (design)

Planning artifact - no firmware changes ride this commit. Decisions
recorded here; implementation phased below.

Provenance: wh-link v1 authoritative bundle (header-wins authority;
`wh_link.h` = spec + reference implementation). Conformance suite
executed on the WHM build host 2026-09-08: **all stages pass**
(crypto KATs -Og/-O2, protocol tests, 84/84 pyca cross-check,
header-split, C++). Watch-agent implementation guidance adopted via
RING-BLE-REVIEW.md (the in-bundle consultation): proven GATT
contract, MTU discipline, Phase-2 reconnect notes.

## 1. Law: BLE is a control channel, never data or timing

The S3 time-slices one 2.4 GHz radio between WiFi and BLE. WHM's
soul rides WiFi: TSF (the walker's spine), whmcast deadlines, the
DJ stream, OTA. Therefore wh-link on a panel carries commands,
status, and pairing - nothing bulk, nothing the show's timing
depends on. TIMESYNC is answered (from our SNTP-disciplined clock,
advisory to the asker) but NEVER disciplines panel time inbound.

Acceptance test for every wh-link phase: a fleet walker soak with a
BLE session active and chatting - `walk` must hold snap~=0 and TSF
drift must stay flat. If BLE moves those numbers, the phase does
not ship. Advertising is suspended while an OTA pull runs
(s_otui.active) - the 3 MB stream owns the radio.

## 2. Transport: the ring's proven GATT contract, verbatim

Panels are NimBLE **peripherals** wearing the wh-link GATT server
exactly as the ring built it and the watch central confirmed it:

- Service `6e574800-...`; RX `6e574801` (WRITE | WRITE_NO_RSP);
  TX `6e574802` (notify, CCCD).
- **Service UUID in the ADV, name in the scan response** - passive
  centrals (the wardriving watch) see us without scan requests.
- Preferred MTU 517; the central initiates the exchange; >=253
  means every <=250 B wh-link frame is one PDU - no BLE-side
  reassembly. sdkconfig must carry
  `CONFIG_BT_NIMBLE_ATT_PREFERRED_MTU` + msys/ACL sizing (watch
  review note 2: `ble_att_set_preferred_mtu` silently caps).
- Re-advertise after disconnect + rotating pseudonym = Phase 2,
  symmetric with the ecosystem's own plan (watch review note 1).

## 3. Core: wh_link.h verbatim, callbacks only

`components/whlink/` vendors the bundle unmodified; `verify.sh`
joins the release gate (the suite must pass on the build host
before any wh-link-touching release). One TU defines
`WH_LINK_IMPLEMENTATION` (main/whlink_glue.c). Config:

- rng = esp_fill_random; now_ms = esp_timer/1000
- transport_send = TX-notify; inbound GATT writes -> wh_on_frame
- role = WH_ROLE_GENERIC + nick = node name (interim; see §6)
- caps = WIFI|BLE|SD|RTC|AUDIO|DISPLAY|IMU (panel truth)
- crypto = NULL (reference backend; AES-GCM swap stays open
  upstream)

## 4. Message bindings — one grammar, three transports

- **CONSOLE 0x2F** ("tunnel a serial-console command to the peer" -
  the spec pre-blessed our thesis): payload = one console line ->
  `whm_console_exec`. Serial, whmcast, and BLE speak the SAME
  grammar; a `fleet ...` line arriving by BLE re-enters the
  existing deadline machinery untouched (one-timestamp doctrine).
  **Sealed-only**: CONSOLE is refused before pairing completes.
- **CONSOLE_OUT 0x30**: bounded capture of the command's output
  (ring buffer, chunked, EVENT-flagged).
- **STATUS 0x62** (implementer-defined body, kept small, EVENT):
  panel brief - fw version, UI-role, fresh/stale peers, pattern,
  walk ok/snap, ip. Emitted on change + slow heartbeat: the fleet
  indicator, on a wrist.
- **ANNOUNCE**: role/caps/nick beacon per spec lifecycle.
- Unknown types: NAK, per spec - mixed-version piconets degrade
  gracefully.

## 5. Pairing UX: SAS on a 64x64 stage

`on_sas` -> six gold digits center-panel (the display IS the
trusted screen); BOOT single-tap = digits-match confirm, long-press
= reject. Discoverable advertising ONLY during an explicit pairing
window (`blepair` console command opens it, time-boxed) - the
RENDEZVOUS privacy doctrine: a matrix that beacons "control me"
all day is a surveillance signal of its own. Phase 2 adds NVS
bonded root key + key-derived rotating pseudonym for silent
reconnect.

## 6. Reconciliation items (spec §6 additive process)

Propose to the ring/watch agents before shipping enums:
- `WH_ROLE_PANEL = 4` (next free after SUBGHZ=3).
- `WH_CAPS_PANEL` as §3 above (no new cap bits needed).
- No new message types required - CONSOLE + STATUS cover the panel
  surface. Interim: GENERIC + nick, spec-blessed.

## 7. Phases

- **L0 vendor + gate**: bundle in-tree, verify.sh green in the
  build container (already demonstrated), no runtime yet.
- **L1 link-up**: NimBLE server + ANNOUNCE + pairing + SAS screen.
  Flash-and-confirm vs the watch. Gate: coex soak clean.
- **L2 console bridge**: CONSOLE/CONSOLE_OUT, sealed-only. The
  wrist drives `pattern`, `walk speed`, `fw test`.
- **L3 telemetry**: STATUS brief + heartbeat.
- **L4 trust**: NVS bonding, rotating pseudonym, re-advertise;
  merges with P4 (whmcast HMAC / signed OTA) into one auth story.
- **L5 reconciliation**: WH_ROLE_PANEL upstream, redistribute the
  shared header per the checklist.

## 8. Roadmap fit

Lands as **P1.5**, after P1's input architecture - a BLE button is
just another type-10 input source once that path exists. The
provisioning slice (hand a headless unit its SSID over BLE) is
independent enough to pull earlier if wanted. P4 partially folds
into L4.
