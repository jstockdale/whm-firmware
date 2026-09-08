# wh-link — authoritative protocol bundle

This is the collected, **authoritative** definition of the Whitehat device-to-device link
protocol (**wh-link**), assembled as the single baseline for implementers. It is the target a new
node — e.g. the CC1101 / EvilCrow V2 sub-GHz collector — builds against, and the reference we
reconcile the Ring's implementation against into a unified version.

Protocol version: **wh-link v1** (`WH_PROTO_VERSION == 1`).
Baseline status at bundling: **all conformance stages pass** (crypto KATs at -Og/-O2, protocol
tests, cross-check vs pyca/cryptography = 84/84, header-split link, C++ includability).

---

## 1. What is authoritative here

Authority runs code-first, not doc-first:

- **`wh_link.h`** — the reference implementation *and* the wire specification. It is a
  framework-neutral, header-only C11 core, shared **verbatim** by the Ring (ESP32-C6) and Watch
  (ESP32-S3) and compiled on the host for verification. Where any prose disagrees with this
  header, **the header wins.**
- **`wh_link_ref_crypto.h`** — the reference crypto backend (ChaCha20-Poly1305 AEAD, X25519,
  HKDF/commitment primitives). A node may substitute a platform backend, but it must produce
  byte-identical results — which the cross-check stage enforces.
- **The conformance suite** (`verify.sh` and friends) — the executable contract. An
  implementation is conforming **iff** it passes this suite against the shared header.

The `.md` specs below are **orientation**, written earlier than the current header and useful for
understanding intent, but they may lag the code. Do not implement from them alone.

- `SPEC-ring-link.md` — the narrative protocol spec (framing, taxonomy, envelope, pairing).
- `RENDEZVOUS.md` — the pairing / rendezvous flow (X25519 AKE + numeric-comparison SAS).
- `README.md` — library overview and build notes.

`correspondence/` holds the historical Ring↔Watch negotiation (proposals, reviews). It is **not**
authoritative — it is context for the reconciliation, and the CC1101 agent can ignore it.

---

## 2. Protocol at a glance

- **Framing**: fixed **22-byte header** + payload (+ **16-byte AEAD tag** when sealed). All
  multi-byte wire integers are **little-endian**, written/read with explicit byte ops (never a
  struct memcpy), so the wire format is host-endianness-independent.
- **Message taxonomy**: session/link, command/RPC, and telemetry groups (see §4).
- **Crypto envelope**: ChaCha20-Poly1305 AEAD, **per-direction keys**, deterministic 96-bit
  nonce = `prefix(8) || counter(4)`.
- **Pairing (AKE)**: X25519 ephemeral key agreement with a commitment-based **numeric-comparison
  SAS** (the on-screen digits you confirm), giving MITM resistance without a shared secret.
- **Replay protection**: 64-entry sliding window, per direction.
- **Fragmentation**: up to 255 offset-addressed fragments per message.

---

## 3. Implementing a node

Include the core in exactly one translation unit:

```c
#define WH_LINK_IMPLEMENTATION      // in ONE .c file
#include "wh_link.h"
```

The platform supplies a small callback set via `wh_link_cfg` (see the struct in `wh_link.h`):

- `rng(user, buf, len)` — cryptographically secure random bytes (on ESP32: `esp_fill_random`).
- `transport_send(user, frame, len)` — hand a framed buffer to the link (BLE GATT write, etc.).
  Return 0 on success.
- a **monotonic clock** source (ms) for timers/replay.
- event callbacks for delivered messages.
- optionally a **crypto backend**; omit to use `wh_link_ref_crypto.h`.
- `user` — opaque pointer passed back to every callback.

Inbound frames from the transport are fed to the core's receive path; the core handles framing,
the crypto envelope, replay, reassembly, and dispatches decoded messages to your callback.

Lifecycle: **ANNOUNCE** (beacon role/caps/batt/nick) → **pair** (PAIR_REQ/RESP/NA/NB, confirm the
SAS) → sealed session (all subsequent traffic AEAD-sealed with per-direction keys).

---

## 4. Message vocabulary (`wh_msg_type`)

Session / link:
`ANNOUNCE 0x01`, `PAIR_REQ 0x02`, `PAIR_RESP 0x03`, `PAIR_NA 0x04`, `PAIR_NB 0x05`,
`PING 0x06`, `PONG 0x07`, `CAPS_GET 0x08`, `CAPS 0x09`, `TIME_SYNC 0x0A`, `STEALTH_SET 0x0B`,
`NAK 0x0C`, `ACK 0x0D`.

Command / RPC:
`SCAN_REQ 0x20`, `SCAN_RESULT 0x21`, `SNIFF_START 0x22`, `SNIFF_STOP 0x23`, `GPS_GET 0x24`,
`GPS_FIX 0x25`, `LOG_WRITE 0x26`, `LOG_ACK 0x27`, `LORA_TX 0x28`, `LORA_RX_SUB 0x29`,
`NFC_REQ 0x2A`, `NFC_RESP 0x2B`, `FILE_LIST 0x2C`, `FILE_GET 0x2D`, `FILE_CHUNK 0x2E`,
`CONSOLE 0x2F`, `CONSOLE_OUT 0x30`, `TIMESYNC_REQ 0x31`, `TIMESYNC_RESP 0x32`.

HID proxy: `HID_PROXY 0x42`, `HID_PROXY_ACK 0x43`.

Telemetry:
**`DEVICE_SEEN 0x60`** (live feed of a find), `THREAT_EVENT 0x61`, `STATUS 0x62`, `LORA_RX 0x63`,
`LAN_NET 0x64`, `LAN_CLIENT 0x65`.

**The find path** for any collector is `DEVICE_SEEN`. Its `wh_seen_kind` today is
`WIFI_AP=1`, `BLE=2`, `154=3`.

---

## 5. Conformance

```
./verify.sh
```

Stages (all must pass):
1. reference crypto KATs (-Og and -O2),
2. protocol tests (`test_protocol.c` — framing, envelope, pairing, replay, fragmentation),
3. cross-check reference crypto vs pyca/cryptography,
4. header-only split (one implementation TU, many user TUs),
5. C++ includability.

A new node's codec is conforming when it round-trips the vectors in `test_vectors.c` /
`dump_vectors.c` and passes the above against the **unmodified** shared header.

---

## 6. Integrating the CC1101 / EvilCrow V2 sub-GHz node

The EvilCrow V2 is ESP32-based with 2× TI CC1101, so it joins the piconet exactly like the Ring:
it is another **collector peer** speaking wh-link over a BLE transport, pairing via X25519+SAS,
and reporting finds via `DEVICE_SEEN`. It uses `wh_link.h` **verbatim** and implements the
callbacks in §3 (esp RNG, esp_timer clock, its BLE write as `transport_send`).

Three **additive, v1-compatible** protocol extensions are needed (all reconciliation items — agree
them with the Ring agent before anyone ships, so the enums don't collide):

1. **Role** — add `WH_ROLE_SUBGHZ` (next free value after `WATCH=2`), or use `WH_ROLE_GENERIC=0`
   until we decide. Roles today: `GENERIC=0, RING=1, WATCH=2`.
2. **Capability bit** — add `WH_CAP_SUBGHZ (1u << 12)`; bits 0–11 are taken (WIFI…IMU). The node
   advertises this in `ANNOUNCE` so the watch can label it.
3. **Find kind + payload** — add `WH_SEEN_SUBGHZ` (kind 4) to `wh_seen_kind`, and define the
   sub-GHz find payload for `DEVICE_SEEN`: at minimum frequency (Hz), modulation (2-FSK/GFSK/OOK/
   MSK…), data-rate, RSSI/LQI, and a bounded decoded-bytes field. This is the one substantive new
   codec and deserves its own vector set once agreed. (Decide whether one kind + a modulation
   field, or per-modulation kinds — I lean one kind + fields.)

Everything else — the crypto envelope, pairing, framing, replay, fragmentation — is unchanged and
reused as-is. Existing nodes NAK unknown kinds gracefully, so a mixed-version piconet degrades
safely during rollout.

---

## 7. Reconciliation checklist (with the Ring agent)

Compare the Ring's current `wh_link.h` against this one and confirm:

- [ ] Same `WH_PROTO_VERSION` (**1**) — a version bump on either side breaks interop.
- [ ] Identical `wh_msg_type` set and numeric values (no locally-added messages that collide).
- [ ] Identical `wh_seen_kind`, `wh_link_role`, and `WH_CAP_*` bit assignments (this is where the
      three sub-GHz additions must be agreed and applied on **all** sides at once).
- [ ] Identical framing constants (22-byte header, 16-byte tag) and the little-endian rd_/wr_
      helpers.
- [ ] Identical crypto parameters (ChaCha20-Poly1305, per-direction keys, nonce = prefix(8)||
      ctr(4), 64-entry replay window). The cross-check stage already pins the primitives; confirm
      the Ring uses the same backend or a byte-identical one.
- [ ] The Ring's build passes **this** `verify.sh` unmodified.

Where the Ring has diverged, we fold the differences back into one header, re-run the suite, and
redistribute that header verbatim to Ring, Watch, and the new CC1101 node.

---

## 8. Bundle manifest

Core (authoritative):
- `wh_link.h` — the shared protocol core / wire spec.
- `wh_link_ref_crypto.h` — reference crypto backend.

Conformance (authoritative contract):
- `verify.sh` — runs everything below.
- `test_protocol.c`, `test_vectors.c`, `test_crypto_kat.c`, `dump_vectors.c`, `crosscheck.py`.
- `impl_tu.c`, `user_tu.c`, `cpp_impl.cpp`, `cpp_include.cpp` — split/C++ build scaffolding.

Orientation (may lag the code; the header wins):
- `SPEC-ring-link.md`, `RENDEZVOUS.md`, `README.md`.

Historical, non-authoritative:
- `correspondence/` — Ring↔Watch proposals and reviews, for reconciliation context only.
