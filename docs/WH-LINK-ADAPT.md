# wh-link on WHM — piconet citizenship over BLE (design, v2)

v2 supersedes v1's flat control-only law with a TIERED timing
doctrine (owner direction: panels are full piconet citizens; BLE
carries state, data, and control; BLE-only and hybrid fleets are
supported outcomes, not accidents).

Planning artifact - no firmware changes ride this commit. Decisions
recorded here; implementation phased below.

Provenance: wh-link v1 authoritative bundle (header-wins authority;
`wh_link.h` = spec + reference implementation). Conformance suite
executed on the WHM build host 2026-09-08: **all stages pass**
(crypto KATs -Og/-O2, protocol tests, 84/84 pyca cross-check,
header-split, C++). Watch-agent implementation guidance adopted via
RING-BLE-REVIEW.md (the in-bundle consultation): proven GATT
contract, MTU discipline, Phase-2 reconnect notes.

## 1. Law v2: the timing TIERS

The S3 time-slices one radio between WiFi and BLE. The law is not
prohibition but hierarchy: BLE must never DEGRADE a tier above it
where that tier exists, and must PROVIDE its own tier where the
higher one does not.

- **Tier 0 - microseconds (WiFi TSF).** Mode A sample-locked
  audio; tightest walker seams. Exists only with WiFi. BLE
  coexistence must leave it untouched: the standing acceptance
  test is a fleet walker soak with a BLE session actively chatting
  - walk holds snap~=0, TSF drift stays flat, or the phase does
  not ship. Advertising suspends during an OTA pull.
- **Tier 1 - milliseconds (wh-link TIMESYNC).** The spec's
  t1/t2/t3 + uncertainty exchange IS an NTP round; over a BLE
  connection with median filtering it delivers ~2-10 ms wall
  agreement, refinable toward sub-ms by CONNECTION-ANCHOR
  timestamping (a BLE connection event is a shared physical
  instant both radios observe - the same trick TSF plays with
  beacons). Sufficient for: deadline fleet commands (333 ms lead),
  the fireworks/nightly scripts, clock screens, and the walker -
  which is STEP-INDEXED by design, so ms-class anchor skew appears
  only as sub-pixel seam phase between panels. NOT sufficient for
  Mode A audio (stays Tier 0 forever).
- **Tier 2 - bulk (wh-link fragmentation + app chunking).** Media
  and OTA as a slow emergency path (~30-60 KB/s realistic on 2M
  PHY; a 3 MB image in minutes, not seconds). Never the routine
  path where WiFi exists.

A panel discipline rule replaces v1's inbound-time refusal: a
WiFi-connected panel answers TIMESYNC but keeps SNTP/TSF as its
own truth; a WiFi-ORPHAN panel accepts wh-link TIMESYNC as its
wall-clock discipline (Tier 1 is its best available tier).

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
- **WH_MSG_WHMCAST (proposed additive, next free telemetry id)**:
  a VERBATIM whmcast datagram tunneled in wh-link. This is the
  entire fleet-over-BLE design in one message: types 2/7/8/9 -
  deadline commands, takeover scripts, Mode-A beacons (ignored off
  Tier 0), walker keyframes - ride the BLE leg unchanged. The
  deterministic replicas neither know nor care which radio
  delivered a keyframe; exec_at still governs; the one-timestamp
  doctrine survives because TIMESYNC supplies the alignment that
  makes deadlines meaningful.
- **Ecosystem inbound (panels as the piconet's public display
  surface)**: DEVICE_SEEN renders as a live find ticker (the
  EvilCrow's sub-GHz catches scrolling on a matrix); THREAT_EVENT
  interrupts with a red flash + reason; GPS_FIX may auto-set
  timezone (opt-in); HID_PROXY becomes a P1 walker input source
  from the watch. Panels consume telemetry as display content and
  emit STATUS in return.
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

## 6. Topologies

- **WiFi fleet (today)**: Tier 0 everywhere; wh-link adds control,
  status, and ecosystem citizenship.
- **Hybrid (the bridge)**: a WiFi-orphan panel pairs by BLE to the
  nearest WiFi panel, which becomes its BRIDGE: relays
  WH_MSG_WHMCAST both ways and serves TIMESYNC from its
  SNTP/TSF-disciplined clock. The orphan runs Tier 1: full fleet
  membership (commands, scripts, walker at relaxed seams), no
  Mode A. Bridge selection: the orphan's paired peer; multi-orphan
  = star on one bridge (NimBLE central+peripheral concurrently on
  the S3).
- **BLE-only fleet (value-add)**: no AP at all. One panel elected
  BLE-conductor (the conduct/join model transposed): central to
  the others, TIMESYNC master from its RTC/SNTP-last-known,
  WH_MSG_WHMCAST relay hub. Everything but Mode A works, seams
  relax to sub-pixel wobble. This is the take-it-anywhere mode -
  a fleet in a field.

## 6b. Pairing topology (owner model, confirmed)

BONDS and SESSIONS are different things.

- **Bonds** (who has paired; NVS, Phase 2): EVERYONE WITH
  EVERYONE, established once at commissioning - watch <-> every
  panel, panels <-> each other. The SAS ritual needs a human, so
  it is done on the bench (N panels + watch = N(N+1)/2 ceremonies,
  minutes, once), then held with rotating pseudonyms so
  reconnection is silent. Pre-existing bonds are what let the
  failure modes need no hands: a WiFi-orphan connects to its
  bonded neighbor and becomes the hybrid case; a fleet in a field
  wakes its BLE star without ceremony.
- **Sessions** (who is connected now): in a healthy WiFi fleet,
  exactly ONE - watch <-> the LEAD (anchor or conductor). The lead
  fans CONSOLE fleet lines out over whmcast with zero extra hops
  and serves the AGGREGATED fleet brief in STATUS. Panel <->
  panel sessions stay dormant (whmcast outclasses them; the
  Tier-0 law forbids spending radio on redundancy) and activate
  only for the hybrid bridge or BLE-only modes.
- **Any-panel entry point (free property)**: fleet commands are
  location-independent - the deadline machinery does not care
  which panel injects a fleet line - so if the lead is absent the
  watch connects to whoever is awake and loses only aggregation.
  Lead preference is an optimization, not a requirement.
- Ladder consequence: L4 (NVS bonding + pseudonyms) rises - it is
  the enabler of paired-in-advance / connect-on-demand. L3 STATUS
  gains a lead flag so the watch finds its preferred partner.

## 7. Reconciliation items (spec §6 additive process)

Propose to the ring/watch agents before shipping enums:
- `WH_ROLE_PANEL = 4` (next free after SUBGHZ=3).
- `WH_CAPS_PANEL` as §3 above (no new cap bits needed).
- **`WH_MSG_WHMCAST`** (next free id, suggested 0x68): payload =
  one verbatim whmcast datagram; deserves a vector set per the
  sub-GHz precedent.
- Interim: GENERIC + nick, spec-blessed.

## 8. Phases

- **L0 vendor + gate**: bundle in-tree, verify.sh green in the
  build container (already demonstrated), no runtime yet.
- **L1 link-up**: NimBLE server + ANNOUNCE + pairing + SAS screen.
  Flash-and-confirm vs the watch. Gate: coex soak clean.
- **L2 console bridge**: CONSOLE/CONSOLE_OUT, sealed-only. The
  wrist drives `pattern`, `walk speed`, `fw test`.
- **L3 telemetry**: STATUS brief + heartbeat.
- **L4 trust**: NVS bonding, rotating pseudonym, re-advertise;
  merges with P4 (whmcast HMAC / signed OTA) into one auth story.
- **L5 tunnel + tiers**: WH_MSG_WHMCAST both directions;
  wh-link TIMESYNC discipline for orphans (Tier 1 clock law);
  bridge relay; acceptance = hybrid walker soak, seams observed.
- **L6 BLE-only conductor**: elected BLE star, fleet-in-a-field
  mode; acceptance = full nightly script over pure BLE.
- **L7 ecosystem display**: DEVICE_SEEN ticker, THREAT_EVENT
  interrupt, HID_PROXY into the P1 input path.
- **L8 reconciliation**: WH_ROLE_PANEL + WH_MSG_WHMCAST upstream,
  redistribute the shared header per the checklist.

## 9. Roadmap fit

Lands as **P1.5**, after P1's input architecture - a BLE button is
just another type-10 input source once that path exists. The
provisioning slice (hand a headless unit its SSID over BLE) is
independent enough to pull earlier if wanted. P4 partially folds
into L4.
