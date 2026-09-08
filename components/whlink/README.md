# wh-link — Whitehat device-to-device link protocol (v1, P0)

Framework-neutral, header-only C11 protocol library for the encrypted link
between the **Whitehat Ring** (ESP32-C6) and the **Whitehat Watch** (ESP32-S3).
Same header compiles verbatim into both firmwares (ESP-IDF v5.4.1), as C or C++,
and on the host for verification. The core references no ESP-IDF/Arduino headers;
the platform supplies transport, RNG, a clock, and (optionally) a crypto backend
through callbacks.

This is the **P0** deliverable: the wire protocol, crypto envelope, pairing
handshake, replay protection, and fragmentation — fully implemented and tested.
Radio bring-up and the feature RPCs (scan/GPS/LoRa/NFC/file transfer) sit on top
of this in later phases; their type IDs are already reserved in the taxonomy.

## What's here

| File | Purpose |
|------|---------|
| `wh_link.h` | Protocol: framing, message taxonomy, envelope, pairing, replay, fragmentation, context/API |
| `wh_link_ref_crypto.h` | Reference primitives: SHA-256, HMAC, HKDF, ChaCha20-Poly1305, X25519 (portable, KAT-tested) |
| `test_crypto_kat.c` | Crypto known-answer tests (RFC 4231 / 5869 / 7748 / 8439, NIST) |
| `test_protocol.c` | Protocol tests: framing vectors, pairing, session, replay, tamper, MITM, fragmentation |
| `dump_vectors.c` + `crosscheck.py` | Independent cross-check of the C crypto against Python `cryptography` |
| `impl_tu.c` / `user_tu.c` | Header-split link check (one impl TU, many user TUs) |
| `cpp_include.cpp` / `cpp_impl.cpp` | C++ includability checks |
| `verify.sh` | Fail-fast build + verify harness (deps checked up front; `-Werror`) |

Run everything:

```sh
./verify.sh
```

Stages: crypto KATs (-Og,-O2) → protocol tests (-Og,-O2) → cross-check vs pyca →
header-split link → C++ includability. All must pass with zero warnings.

## Wire format

Fixed 22-byte header, all multi-byte integers **little-endian** (explicit byte
serializers — endianness-safe regardless of host; one-line change to big-endian):

```
off  field         size  notes
0    magic         2     0x57 0x48  "WH"
2    version       1     = 1
3    type          1     wh_msg_type
4    flags         1     ENC|REQ|RESP|EVENT|ACK_REQ (bits 5-7 reserved 0)
5    reserved      1     MUST be 0 in v1
6    src_id        2     sender short id
8    dst_id        2     0xFFFF = broadcast
10   seq           4     per-direction frame counter (also AEAD nonce input)
14   corr          2     RPC correlation id (0 = none)
16   msg_id        2     logical message id (fragment grouping)
18   frag_index    1     0 .. frag_total-1
19   frag_total    1     >=1 (1 = unfragmented)
20   payload_len   2     this fragment's payload length
--   header = 22 bytes  --
22   payload       payload_len
+    tag           16    present iff flags & ENC
```

ESP-NOW MTU is 250, so the max fragment payload is 212 (encrypted) or 228
(plaintext). Up to 255 fragments/message.

## Crypto envelope

- **AEAD:** ChaCha20-Poly1305 (RFC 8439). AAD = the 22-byte header, so every
  header field is authenticated. Algorithm is pluggable via the `wh_crypto_if`
  backend + an algorithm id, so AES-256-GCM (mbedTLS, hardware-accelerated on
  ESP32) is a drop-in swap — see "Open decisions".
- **Directional keys.** Pairing derives, via HKDF-SHA256 from the ECDH shared
  secret, `key_i2r`, `key_r2i` and 8-byte nonce prefixes `np_i2r`, `np_r2i`.
  Each direction has its own key + counter, so cross-direction nonce collision
  is impossible by key separation.
- **Nonce = prefix(8) ‖ counter(4, little-endian).** The counter is the wire
  `seq`, reset to 0 at session establishment, incremented per frame. No reuse by
  construction. Rekey (re-pair) before the 32-bit counter wraps.

## Pairing — X25519 + commitment numeric-comparison SAS

Authenticated key exchange with a short out-of-band check, defeating an active
MITM (this is the Bluetooth LE Secure Connections "numeric comparison"
construction, done explicitly):

```
I -> R  PAIR_REQ  { PKi }                    ephemeral X25519 public
R -> I  PAIR_RESP { PKr, Cr }                Cr = HMAC(Nr, "wh-cmt"‖PKr‖PKi)  -- commit to Nr
I -> R  PAIR_NA   { Ni }                      initiator reveals nonce
R -> I  PAIR_NB   { Nr }                      responder reveals nonce (opens commitment)
        I verifies Cr; both compute
        SAS = SHA256("wh-sas"‖PKi‖PKr‖Ni‖Nr) mod 1e6   -> 6 digits
        user compares the digits on both screens, then wh_pair_confirm()
```

The commitment binds `Nr` before `Ni` is revealed, so neither side can grind its
nonce to force a matching SAS. A MITM must run two separate handshakes with
different ephemerals, which yields different SAS on each device — the user sees a
mismatch and rejects. Session keys are derived and used only after both users
confirm.

Handshake frames (`ANNOUNCE`, `PAIR_*`) are sent in plaintext (they carry only
public values); their integrity comes from the SAS, not from encryption.

## Replay protection

Per-direction 64-entry sliding window over the 32-bit `seq`. On receive: the
window is pre-checked (cheap reject of clearly-old/duplicate frames) **before**
AEAD; the window is advanced **only after** the tag verifies — so a forged frame
can neither be replayed nor used to desync the window.

## Fragmentation

Offset-addressed: non-final fragments are exactly the per-mode cap; the receiver
places fragment *i* at `i × cap`, tracks a bitmap + the final length, and
delivers when all fragments arrive (robust to reorder). One in-flight inbound
message at a time (sufficient for a point-to-point control link); bulk transfers
(files, screenshots) chunk at the application layer into messages that each fit
`WH_REASM_MAX` (default 4096).

## API sketch

```c
wh_config cfg = {0};
cfg.local_id = 0x0001; cfg.peer_id = 0x0002;   // peer via ANNOUNCE discovery
cfg.role = WH_ROLE_RING; cfg.caps = WH_CAPS_RING;
cfg.rng = my_rng;                 // esp_random / mbedtls CTR-DRBG
cfg.now_ms = my_now_ms;           // esp_timer
cfg.transport_send = my_espnow_send;
cfg.on_message = my_on_message;   // decrypted, reassembled app messages
cfg.on_sas = my_show_sas;         // display 6 digits for the user
cfg.on_paired = my_on_paired;
cfg.crypto = NULL;                // NULL -> reference; or an mbedTLS backend
wh_ctx ctx; wh_ctx_init(&ctx, &cfg);

// receive path: hand every raw ESP-NOW/BLE frame to:
wh_on_frame(&ctx, frame, len);

// send (auto encrypt if paired, auto fragment):
wh_send(&ctx, WH_MSG_GPS_FIX, WH_FLAG_EVENT, 0, buf, n);

// pairing (initiator):
wh_pair_start(&ctx);              // ... on_sas fires on both devices ...
wh_pair_confirm(&ctx, digits_match);
```

## Integrating on ESP-IDF v5.4.1

- **RNG:** `esp_fill_random` (or mbedTLS CTR-DRBG) in the `rng` hook.
- **Clock:** `esp_timer_get_time() / 1000` in `now_ms`.
- **Transport:** `esp_now_send()` in `transport_send`; feed the `esp_now_recv_cb`
  payload straight into `wh_on_frame`. Ring/Watch coordinate the ESP-NOW channel
  out of band (see SPEC-watch.md).
- **Crypto backend (optional):** implement `wh_crypto_if` over mbedTLS to use the
  ESP32 AES accelerator (AES-256-GCM) and the vetted stack. The reference
  backend remains the portable default and the oracle the tests validate against.
- The ring's known WiFi-scan-after-802.15.4 coexistence bug (SPEC §15) is exactly
  why the link is designed to survive one side going quiet; the Watch can also
  instrument it.

## Verification status (P0)

- 24 crypto KATs against published RFC/NIST vectors — pass.
- 84 independent agreements with pyca/`cryptography` on varied non-KAT inputs — 0 mismatches.
- 39 protocol assertions: header byte-exactness + validation, message
  round-trips, replay window, nonce construction, full pairing handshake (SAS
  agreement, key mirroring), session encrypt/decrypt, replay + tamper rejection,
  MITM/handshake-tamper detection, fragmentation reassembly — pass.
- Clean at `-Og` and `-O2` with `-Werror` under the strict warning set; compiles
  as C and C++; header-split links.

## Open decisions (do not block P0)

1. **AEAD:** AES-256-GCM (hardware on ESP32) vs ChaCha20-Poly1305 (reference).
   Pluggable via backend + algorithm id.
2. **LoRa relay framing:** raw wh-link vs Meshtastic-compatible (reuse meshtastic-lite).
3. **SD log format** (Kismet/CSV/GPX) + at-rest encryption default (rfcensus
   Argon2id / AES-GCM-SIV pattern).
4. **Identity persistence / fast re-pair:** store a root key in NVS (wrapped) to
   skip SAS on reconnect (trust-on-first-use). Struct hooks are in place.

## License

BSD-3-Clause (intended), matching the surrounding Whitehat firmware. The
algorithms are public standards.
