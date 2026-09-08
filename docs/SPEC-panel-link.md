# SPEC — WHM panel over wh-link (for the watch agent)

The WHM matrix panels (ESP32-S3, 64x64 HUB75) are wh-link piconet
citizens as of firmware v0.40.0. This is the integration contract:
what a central (the watch) can rely on today, byte-exact, plus what
is committed next. Authority: this document for panel-specific
bodies; wh_link.h for everything the shared header defines.

## 1. Transport (identical to the ring's contract)

NimBLE peripheral. Service 6e574800-a9c4-4b7e-9d21-5748c0de0001;
RX 6e574801-... (WRITE | WRITE_NO_RSP), TX 6e574802-... (NOTIFY +
CCCD). Service UUID in the ADV (passive scan sees us), node name in
the scan response. MTU preferred 517, central initiates; at >=253
every frame is one PDU. Discoverable ONLY inside an explicit
pairing window (console `ble pair [secs]`, default 120) - the
RENDEZVOUS privacy doctrine. Advertising yields to an active OTA.

## 2. Identity

role = WH_ROLE_GENERIC (interim; WH_ROLE_PANEL=4 proposed, spec §6
process). caps = WIFI|BLE|SD|RTC|AUDIO|DISPLAY|IMU. nick = the
panel's fleet node name ("One", "Two"...). local_id = BT MAC low16.
ANNOUNCE fires when the central subscribes to TX.

## 3. Pairing

Spec-standard X25519 + commitment SAS. The six digits render ON THE
PANEL (gold, size-2) with `TAP YES / HOLD NO`; the BOOT button
decides. Sessions are per-boot today; NVS bonding + rotating
pseudonym land at L4 (committed).

## 4. CONSOLE 0x2F / CONSOLE_OUT 0x30 — the bridge

- Payload: ONE console line, UTF-8, length-delimited (<=199 bytes).
  No operator chaining (`;`, `&&`) - one command per message.
- SEALED-ONLY: an unsealed CONSOLE is dropped (logged panel-side),
  never executed, never answered.
- Reply: CONSOLE_OUT, WH_FLAG_RESP, same corr. Body is a short
  ASCII ack: `ok` | `rc <n>` (command ran, nonzero exit) |
  `err <esp_err>` | `unknown` | `busy` (queue full, depth 4).
- Design truth: the panels ARE stdout for a display fleet - the
  command's real output is the show. Rich state rides STATUS.
- The whole panel grammar is available (`help` lists it). Notably:
  `pattern <name>`, `walk speed <r>`, `fleet <line...>` (fans out
  over whmcast to the WHOLE fleet - any bonded panel is a valid
  entry point), `fw test`, `rtc set ...`, `ble off`.

## 5. STATUS 0x62 — the panel brief (v1 body, 19 bytes LE)

```
off 0  ver        u8   = 1
    1  flags      u8   bit0 = LEAD (elected anchor or conductor)
                       bit1 = conductor specifically
    2  ui_role    u8   0 follower, 1 elected anchor, 2 conductor
    3  fresh      u8   peers heard < 8 s
    4  stale      u8   peers 8-20 s
    5  fw[8]      char firmware version, NUL-padded
   13  uptime_s   u32
   17  heap_kb    u16  free internal heap
```
Emitted: on pairing seal, after every bridged command, 10 s
heartbeat. WH_FLAG_EVENT. Pick your session partner by bit0 when
you can see several panels: the LEAD aggregates fleet truth
(fresh/stale are its live peer table) and fans `fleet` lines with
zero extra hops - but any panel works.

## 6. Committed next (design: docs/WH-LINK-ADAPT.md)

- L4: NVS bonds (everyone-with-everyone at commissioning),
  rotating pseudonym, re-advertise after disconnect.
- L5: WH_MSG_WHMCAST (proposed 0x68) - one VERBATIM whmcast
  datagram per message. This is the walker-keyframe feed and the
  hybrid/BLE-only fleet transport. Tier-1 clocking via spec
  TIMESYNC.
- Reconciliation asks, per wh-link §6: WH_ROLE_PANEL=4;
  WH_MSG_WHMCAST. Nothing else needed - CONSOLE + STATUS cover us.

## 7. L4 as shipped (v0.41.0) — bond + pseudonym

- Bond = the shared header's session blob (WH_SESSION_BLOB_LEN),
  saved to NVS on pairing, re-saved every WH_PERSIST_STRIDE sends
  and immediately after every import (the header's own contract).
  Reconnect: central connects, panel imports (tx_ctr jumps by
  WH_PERSIST_MARGIN), session is sealed with NO SAS. Symmetric by
  construction - both ends run the header's byte-identical logic.
  `ble forget` erases the bond.
- Rotating pseudonym (RECONCILIATION PROPOSAL - concrete, adopt or
  counter): outside a pairing window, a bonded panel advertises
  connectable with NO service UUID and NO name; manufacturer data
  = FF FF 'W' 'P' + token[6]. token = HMAC-SHA256(K_pseud,
  LE64(unix_seconds/60))[0..5]. K_pseud = HKDF-SHA256(
  salt="wh-pseud-v1", ikm = tx_key XOR rx_key, 32 out) - the XOR
  makes the ikm side-invariant (my tx is your rx). Accept the
  current and adjacent epoch when resolving. Interval 800-1100 ms.
