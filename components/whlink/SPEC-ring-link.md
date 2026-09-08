# Whitehat Ring ↔ Watch Link — Implementation Spec (Ring side)

**Audience:** the agent implementing the **Ring** firmware (LilyGo T-QT-C6 / ESP32-C6).
**Status:** v1, matches `wh_link.h` `WH_PROTO_VERSION = 1`.
**Companion docs:** `wh_link.h` (canonical protocol + reference crypto), `RENDEZVOUS.md`
(transport/discovery model). The **Watch** side is implemented and its behavior is
validated by a host 2-node sim (`firmware/watch/test/test_link.c`) — read that test; it
is the executable contract the ring must interoperate with.

---

## 0. The one rule that makes this easy

**The ring links the exact same `wh_link.h` (+ `wh_link_ref_crypto.h`) as the Watch, configured as `WH_ROLE_RING`.**
Both peers running the same protocol implementation is what guarantees wire
compatibility — do **not** hand-roll the framing, pairing, or AEAD. Your job is to
provide the platform hooks (RNG, clock, transport) and the ring-specific *behavior*:
advertise, respond to pairing, and stream finds. Everything below is either (a) how to
drive that library or (b) the semantics of the messages the ring must originate or accept.

```c
#define WH_LINK_IMPLEMENTATION   // in exactly one .c file on the ring
#include "wh_link.h"
```

---

## 1. Roles & topology

| | Watch | Ring |
|---|---|---|
| `role` | `WH_ROLE_WATCH` (2) | `WH_ROLE_RING` (1) |
| pairing | **initiator** (calls `wh_pair_start`) | **responder** (never calls `wh_pair_start`) |
| geotag authority | yes (owns GPS) | no — never stamps location |
| durable log | yes (SD, NDJSON is source of truth) | no — asks the Watch to persist |
| Meshtastic keys / mesh state | yes | never |
| primary output | dashboard + storage | **`DEVICE_SEEN` stream** |

`caps` for the ring: `WH_CAPS_RING` = `WIFI | BLE | IEEE802154 | DISPLAY | IMU`.
The ring is a **sensor + remote**; the Watch is the **hub + system of record**. Design
everything around that asymmetry.

Short IDs (`local_id` / `peer_id`) are 16-bit. Pick a stable non-broadcast `local_id`
for the ring (e.g. `0x0002`; the Watch uses `0x0001`). `WH_ID_BROADCAST = 0xFFFF` is
reserved for ANNOUNCE.

---

## 2. Two transports (see RENDEZVOUS.md)

Discovery/pairing and bulk data ride different radios on purpose:

- **BLE** — channel-independent advertising solves the ESP-NOW "which channel?"
  chicken-and-egg. The ring advertises **ANNOUNCE** and the pairing handshake frames
  are carried over BLE (GATT write/notify, or connectionless — implementer's choice, as
  long as whole `wh-link` frames are delivered intact to `wh_on_frame`).
- **ESP-NOW / Wi-Fi** — once paired and both on a common channel, all data
  (`DEVICE_SEEN`, `GPS_FIX`, `TIME_SYNC`, …) rides ESP-NOW for throughput.

`wh-link` is transport-agnostic: it hands you a complete frame in `transport_send`, and
you feed complete received frames to `wh_on_frame`. **Never fragment a `wh-link` frame
across a boundary the peer wouldn't reassemble** — `wh-link` already fragments/reassembles
above the transport (`WH_REASM_MAX = 4096`), so your transport must deliver each
`transport_send` buffer as one `wh_on_frame` call on the other side.

**Channel coordination (ESP-NOW):** the peers must share a Wi-Fi channel. Rule of thumb
for the alpha: the ring follows the Watch. The Watch advertises its current channel in
the post-pairing session (or both default to a fixed channel); ESP-NOW peers are added
with `encrypt = false` because **wh-link supplies its own AEAD** — do not double-encrypt
with ESP-NOW PMK. Confirm the exact channel-follow handshake against the Watch build;
this is the main `[VERIFY-HW]` seam.

**Privacy:** after bonding, do **not** advertise a static "Whitehat gear here" beacon.
Reconnect via a key-derived rotating pseudonym (see §6). Unbonded discovery ANNOUNCE is
fine; a persistent recognizable bond beacon is a tracking leak.

---

## 3. Context setup (ring)

```c
static wh_ctx g_ctx;

static void ring_rng(void *u, uint8_t *buf, size_t n) { esp_fill_random(buf, n); }
static uint32_t ring_now(void *u) { return (uint32_t)(esp_timer_get_time()/1000); }
static int ring_tx(void *u, const uint8_t *frame, size_t len) {
    // BLE while pairing, ESP-NOW once established. Return WH_OK or WH_ERR_TRANSPORT.
}
static void ring_on_sas(void *u, const char *digits) { /* show 6 digits on the display */ }
static void ring_on_paired(void *u, int ok) { /* persist bond (§6) */ }
static void ring_on_msg(void *u, const wh_msg *m) { /* §5 dispatch */ }

wh_config cfg = {0};
cfg.local_id = 0x0002;
cfg.peer_id  = WH_ID_BROADCAST;          // learned at discovery
cfg.role     = WH_ROLE_RING;
cfg.caps     = WH_CAPS_RING;
strncpy(cfg.nick, "ring", sizeof(cfg.nick)-1);
cfg.crypto   = wh_ref_crypto();          // ChaCha20-Poly1305 + X25519 + HKDF-SHA256
cfg.rng = ring_rng; cfg.now_ms = ring_now; cfg.transport_send = ring_tx;
cfg.on_message = ring_on_msg; cfg.on_sas = ring_on_sas; cfg.on_paired = ring_on_paired;
wh_ctx_init(&g_ctx, &cfg);
```

**RNG must be cryptographically secure** (`esp_fill_random`, which is HW-backed once
Wi-Fi/BLE is running). The X25519 ephemeral secret and pairing nonces come from it; a
weak RNG breaks pairing security.

Drive the library from two places only:
- your **receive path** → `wh_on_frame(&g_ctx, frame, len)` for every frame in.
- your **app logic** → `wh_send(...)` / `wh_announce_send(...)` to originate messages.

`wh_on_frame` internally handles decryption, replay, reassembly, **and the entire
responder pairing handshake** — you do not implement PAIR_* by hand.

---

## 4. Pairing (responder role)

X25519 key agreement with a **commitment-based numeric-comparison SAS** (6 digits) that
defeats MITM. Message flow (initiator = Watch, responder = Ring):

```
Watch → Ring : PAIR_REQ  (0x02)  PKi                       [wh_pair_start on Watch]
Ring  → Watch: PAIR_RESP (0x03)  PKr + commitment Cr        [auto, in wh_on_frame]
Watch → Ring : PAIR_NA   (0x04)  Ni                         [auto]
Ring  → Watch: PAIR_NB   (0x05)  Nr  (opens the commitment) [auto]
   ── both compute SAS = 6 digits from (PKi, PKr, Ni, Nr) ──
on_sas fires on BOTH devices → user compares the two 6-digit numbers →
wh_pair_confirm(ctx, matched) on BOTH → session established, on_paired(ok=1) fires
```

Commitment `Cr` binds the responder's nonce before the initiator reveals `Ni`, so neither
side can grind the SAS — a mismatch means tamper/MITM (`WH_ERR_COMMIT`).

**Ring responsibilities during pairing:**
1. Learn the Watch's short id (`wh_ctx_set_peer` from the ANNOUNCE you saw, or accept the
   `src_id` of the incoming `PAIR_REQ`).
2. Feed every pairing frame to `wh_on_frame`. The library emits the responder frames via
   your `transport_send`.
3. When `on_sas(digits)` fires, **display the 6 digits** and let the wearer compare them
   with the Watch's screen. This is the human anti-MITM check — it must be a real
   compare, not auto-accept.
4. On user approval, call `wh_pair_confirm(&g_ctx, 1)` (or `0` to abort). `on_paired`
   then fires; `wh_is_established()` becomes true.

Pairing states you may observe (`wh_pair_state`): `IDLE → R_SENT_RESP → AWAIT_SAS →
ESTABLISHED` (or `FAILED`). Do not send application messages before `ESTABLISHED`.

---

## 5. Messages the ring must handle

`wh_send(ctx, type, flags, corr, body, len)` handles encryption (once established),
fragmentation, and transport automatically. Encode/decode bodies with the `wh_enc_*` /
`wh_dec_*` helpers — **do not** build bodies by hand. Everything post-pairing is
AEAD-sealed (`WH_FLAG_ENC` is applied for you).

### 5.1 Originate

**`ANNOUNCE` (0x01)** — broadcast beacon so the Watch can discover the ring.
`wh_announce_send(ctx, battery_pct, flags)`. Body = `wh_announce`
{proto_version, role, caps, battery_pct, flags (bit0 stealth, bit1 paired), short_id,
nick}. Send periodically **before** bonding (over BLE adv). After bonding, prefer the
rotating-pseudonym reconnect (§6) over a static beacon.

**`DEVICE_SEEN` (0x60) — THE RING'S PRIMARY JOB.** For every Wi-Fi AP / BLE device /
802.15.4 device the ring observes, emit one `DEVICE_SEEN`. Flags:
`WH_FLAG_EVENT` (fire-and-forget; loss is acceptable — the next observation supersedes).

```c
wh_device_seen ds = {0};
ds.kind    = WH_SEEN_WIFI_AP;              // or WH_SEEN_BLE / WH_SEEN_154
ds.rssi    = rssi;                         // int8, dBm
ds.channel = chan;
ds.freq_mhz= freq;                         // 0 if unknown; disambiguates 2.4/5/6 GHz
ds.sec     = sec;                          // 0 unk,1 open,2 wep,3 wpa,4 wpa2,5 wpa3,6 wpa2/3,7 eap
ds.flags   = (randomized_mac ? WH_SEEN_FLAG_RANDMAC : 0)
           | (threat         ? WH_SEEN_FLAG_THREAT  : 0);
memcpy(ds.addr, bssid_or_bleaddr, 6);      // 154: short addr in addr[0..1], rest 0
strncpy(ds.name, ssid_or_blename, sizeof(ds.name)); // NUL-terminated
uint8_t body[128]; int n = wh_enc_device_seen(&ds, body, sizeof(body));
wh_send(&g_ctx, WH_MSG_DEVICE_SEEN, WH_FLAG_EVENT, 0, body, (size_t)n);
```

**Do not geotag.** The ring sends the raw find with **no location**; the Watch stamps its
current GPS fix, a timestamp, and ring provenance (`dev = ring short_id`) at ingest, then
persists it. Send observations as you see them so RSSI-over-time is preserved (mirror the
Watch's own "passive, non-dedup" policy). Address byte order is **MSB-first** in `addr`
(the display convention); if your BLE stack gives LSB-first, reverse before filling `addr`.

**`STATUS` (0x62)** — optional periodic ring health (battery, mode). Body format is
implementer-defined for the alpha; keep it small and `WH_FLAG_EVENT`.

**`THREAT_EVENT` (0x61)** — optional, for detector hits worth surfacing immediately
(e.g. a flagged surveillance device). `WH_FLAG_EVENT` or `WH_FLAG_ACK_REQ` if you want
confirmation.

### 5.2 Accept (from the Watch)

**`TIME_SYNC` (0x0A)** — the Watch's authoritative wall clock (`wh_time_sync`
{unix_seconds, millis, source, valid}). Set the ring's clock from it. `source` is
`WH_TIME_GPS/RTC/SNTP`; trust GPS/SNTP over RTC.

**`GPS_FIX` (0x25)** — the Watch pushes its fix (`wh_gps_fix` {lat_1e7, lon_1e7, alt_mm,
hacc_mm, unix_seconds, millis, speed_mms, heading_cdeg, fix_type, sats}) so the ring can
show position / geofence locally. The ring still never *stamps* finds — this is for
display/logic only.

**`STEALTH_SET` (0x0B)** — mirror go-dark across devices. On stealth-on, the ring must
stop active transmissions (no scan probes, no ANNOUNCE, BLE observer passive only) to
match the Watch. Reflect the state in the `ANNOUNCE` flags bit0 when you do advertise.

**`CAPS_GET` (0x08)** → reply `CAPS` (0x09) with `wh_caps`. **`PING` (0x06)** → reply
`PONG` (0x07). Unknown/unsupported types → reply **`NAK` (0x0C)** with a
`wh_nak_reason` (`UNKNOWN_TYPE`, `NOT_SUPPORTED`, `NO_CAP`, …) echoing the request's
`corr`. Requests (`WH_FLAG_REQ`) expect a `WH_FLAG_RESP` with the same `corr`.

### 5.3 Optional / later (behind caps)

- **`SCAN_REQ` (0x20) / `SCAN_RESULT` (0x21)** — the Watch asks the ring for a one-shot
  scan (vs. the continuous `DEVICE_SEEN` stream).
- **`SNIFF_START` (0x22) / `SNIFF_STOP` (0x23)** — the Watch remotely drives the ring's
  **802.15.4 sniffer** (the ring's unique capability). Stream captures as `DEVICE_SEEN`
  (`kind = WH_SEEN_154`) or a dedicated body.
- **`LOG_WRITE` (0x26) / `LOG_ACK` (0x27)** — the ring asks the Watch to persist a record
  to SD. Body `wh_logrec_msg` {fmt, len, data}; `fmt = WH_LOGREC_FMT_NDJSON` means `data`
  is **one wh-log NDJSON line** produced by `wh_log.h` on the ring. Note the Watch will
  **parse and re-serialize** it canonically before appending — it never writes your bytes
  verbatim — so keep records well-formed. Idempotency is expected (safe to retry).
- **LoRa bridge** (`LORA_TX 0x28`, `LORA_RX_SUB 0x29`, `LORA_RX 0x63`): the **Watch owns
  the entire Meshtastic stack** (channel keys, node DB, hop limit, dedup). The ring
  injects/subscribes via **semantic fields only** (`wh_lora_tx` / `wh_lora_sub` — never a
  pre-built MeshPacket, never raw protobuf). By default the ring may only originate
  `WH_PORT_TEXT` (1) and `WH_PORT_PRIVATE` (256) — structured ports (Position/NodeInfo/
  Telemetry) are Watch-originated so the ring can't spoof them (`wh_lora_port_allowed_default`).
  This is a deliberate two-hop trust boundary: wh-link secures Ring↔Watch, Meshtastic
  channel crypto secures Watch↔mesh, and **no key crosses it**.

---

## 6. Bonding & reconnect

- On `on_paired(ok=1)`, persist the bond so you don't re-pair every boot. `wh-link`
  keeps session state in `wh_session` (tx/rx keys, nonce prefixes, counters, replay
  window) inside `wh_ctx`; persist what you need to re-establish (at minimum the
  long-term keying material your design derives the session from). Store in NVS, never in
  the clear on removable media.
- **Reconnect privacy:** derive a rotating pseudonym from the shared key for the
  reconnect beacon/adv so a passive observer can't recognize "this ring is here" across
  sessions. Do not broadcast a static bonded identifier.
- `wh_unpair(ctx)` forgets everything (user-initiated "forget device").

---

## 7. Wire format reference (for independent verification)

You should not need to encode these by hand (the library does), but for testing:

- **Header:** `WH_HDR_LEN = 22` bytes: version(1), type(1), flags(1), src_id(2),
  dst_id(2), then msg_id / fragment / length fields — see `wh_hdr_encode` /
  `wh_hdr_decode`. **All multi-byte integers are little-endian.**
- **Flags:** `ENC`(1<<0), `REQ`(1<<1), `RESP`(1<<2), `EVENT`(1<<3), `ACK_REQ`(1<<4);
  bits 5–7 reserved = 0.
- **AEAD:** 32-byte key, **12-byte nonce = per-direction prefix ‖ 32-bit counter**,
  16-byte tag (`WH_TAG_LEN`). The header is the AAD; payload is sealed. Counter increments
  per sent frame; the receiver enforces a replay window (`wh_replay`, 64-bit).
- **Crypto suite:** X25519 (key agreement), HKDF-SHA256 (key derivation),
  ChaCha20-Poly1305 (reference/default AEAD). An AES-256-GCM backend (mbedTLS on the C6)
  satisfies the same `wh_crypto_if` contract and is wire-compatible with the same key/
  nonce/tag sizes — selectable later via an algorithm id (reserved in v1; use the
  reference suite for interop now).
- **Error codes:** `wh_err` (`WH_ERR_AUTH` = AEAD fail, `WH_ERR_REPLAY`, `WH_ERR_COMMIT`
  = pairing tamper, `WH_ERR_STATE`, …). `wh_strerr()` renders them.

---

## 8. Security requirements (non-negotiable)

1. **SAS is a real human compare.** Never auto-confirm `wh_pair_confirm`. The commitment
   scheme only protects you if the wearer actually compares the two 6-digit numbers.
2. **CSPRNG only** for `rng` (ephemeral X25519 secret, pairing nonces, pseudonyms).
3. **No plaintext application traffic.** Only handshake frames are unencrypted; everything
   after `ESTABLISHED` is AEAD-sealed. Reject anything that fails auth (the library does;
   don't paper over `wh_on_frame < 0`).
4. **The ring holds no mesh/channel keys** and never emits raw MeshPackets — semantic
   fields through the Watch only, default port policy enforced.
5. **Records the ring sends for logging are advisory** — the Watch validates and
   re-serializes. Correspondingly, treat anything the Watch sends as trusted only after it
   arrives over the established session.
6. **Honor stealth immediately.** `STEALTH_SET` on ⇒ cease active RF that would reveal the
   pair.

---

## 9. Validation checklist (what "done" means for the ring)

- [ ] Links `wh_link.h` unmodified as `WH_ROLE_RING`; RNG is HW CSPRNG.
- [ ] Advertises `ANNOUNCE` over BLE; the Watch discovers it.
- [ ] Completes the responder pairing handshake driven purely by `wh_on_frame`; shows the
      SAS; `wh_pair_confirm` on human match; reaches `ESTABLISHED`.
- [ ] Streams `DEVICE_SEEN` for Wi-Fi + BLE + 802.15.4 finds, **ungeotagged**, correct
      `kind`/`sec`/`flags`/`addr` (MSB-first)/`name`.
- [ ] Accepts `TIME_SYNC`, `GPS_FIX`, `STEALTH_SET`; answers `PING`/`CAPS_GET`; `NAK`s the
      unknown.
- [ ] Persists the bond; reconnects via a rotating pseudonym (no static bonded beacon).
- [ ] Interops with the Watch build: run the equivalent of `test_link.c` against your ring
      context (two contexts, loopback) and confirm SAS match + an encrypted `DEVICE_SEEN`
      decodes on the peer. The Watch's `test_link.c` is the reference; your ring context
      must be a drop-in for the `RING` side of it.

When in doubt, the source of truth is `wh_link.h` and the Watch's `test_link.c` — match
their behavior exactly and interop is guaranteed.
