# Ring ↔ Watch — Discovery, Connection & Credential Sharing (design)

How the two devices find each other, authenticate, connect, and share things
like Wi-Fi credentials. This is the platform/transport layer *around* the
`wh-link` protocol library (which already handles framing, AEAD, SAS pairing,
and the message taxonomy). Design only — implementation lands in the Watch link
phase, validated against the protocol vectors + a host two-node sim before any
on-hardware pairing.

## Roles: BLE discovers + pairs, Wi-Fi/ESP-NOW carries

Both devices have Wi-Fi (2.4 GHz) and BLE. Each is used for what it is best at —
this is "both," not "either/or":

- **BLE = discovery + pairing + fallback control.** BLE advertising rides its own
  channels (37/38/39), *independent of the Wi-Fi/ESP-NOW channel*. That is the
  key property: it breaks the chicken-and-egg where ESP-NOW discovery would
  require both devices to already sit on the same channel. BLE also gives a
  reliable connection-oriented pipe for the multi-round pairing handshake.
- **ESP-NOW = the data session.** Once discovered and channel-aligned, bulk
  telemetry/commands (DEVICE_SEEN streams, GPS_FIX, etc.) flow over ESP-NOW:
  connectionless, low-latency, no AP required. BLE remains as fallback.

## 1. Detection / rendezvous (BLE beacon)

Each device advertises a small Whitehat beacon (fits legacy 31-byte adv;
manufacturer-specific data or a 128-bit service UUID) and simultaneously scans.
Beacon contents:

- magic + version
- device_type (ring / watch)
- short_id (2 bytes)
- caps bitmap (has_gps, has_154, has_lora, has_sd, has_nfc, has_audio, …)
- current ESP-NOW channel (so the peer knows where to reach it)
- flags (pairing-mode active, already-bonded)

Seeing the peer's beacon *is* the rendezvous: you learn it exists, its
capabilities, and which channel to reach it on over ESP-NOW. (This is essentially
the `ANNOUNCE` message, carried over BLE adv.)

## 2. Privacy-preserving rendezvous (matters for the brand)

A static "I am a Whitehat recon device" beacon would itself be a surveillance
signal — the exact thing this product exists to defeat. So:

- **The discoverable Whitehat beacon is advertised only during an explicit,
  time-boxed pairing window** (user-initiated on both devices). Minimal exposure.
- **After bonding, reconnection uses a key-derived rotating pseudonym** — a
  resolvable identifier computed from the shared secret + a time/counter
  (BLE-RPA-style, Whitehat-specific). Only the bonded peer can resolve it; to
  everyone else it is an ordinary random BLE address. The ongoing presence is
  unlinkable and never announces "Whitehat gear here."
- ESP-NOW payloads are already AEAD-encrypted (`wh-link`); randomize the ESP-NOW
  source MAC as well to cut linkability of the data link.

## 3. Connection lifecycle (state machine)

```
 UNPAIRED ── user starts pairing on both ──► BLE GATT connect
    │                                            │
    │  (low-duty advertise + scan)               ▼
    │                                    wh-link SAS pairing
    │                          (x25519 + commitment; 6-digit code shown on the
    │                           Watch, confirmed on the ring)
    │                                            │
    ▼                                            ▼
 BONDED, DISCONNECTED ◄───── link lost ──── CONNECTED (ESP-NOW session)
    │   scan for peer's rotating pseudonym;       │   PING/PONG keepalive;
    │   on match reconnect with stored keys       │   telemetry/commands flow;
    └── align ESP-NOW channel from beacon ───────►│   BLE stays as low-rate
                                                  │   fallback or drops to save
                                                  │   the ring's battery
```

- **Bonding** stores the SAS-derived session keys + peer id/caps in NVS on both
  devices, so reconnection never re-pairs.
- **Link loss** → buffered replay with idempotency keys (no double-logging to SD)
  and graceful degrade: each device keeps working solo.
- **Duty-cycling** the ring (tiny battery): low-duty advertise/scan while
  disconnected; connect opportunistically; no chatty streaming by default.

## 4. Channel coordination (the ESP-NOW constraint)

ESP-NOW is locked to the Wi-Fi STA channel whenever the device is associated. The
beacon advertises each device's current channel; alignment rules:

| Situation                              | ESP-NOW channel                                   |
|----------------------------------------|---------------------------------------------------|
| Both on the same Wi-Fi network         | that STA channel → ESP-NOW direct                 |
| One associated, one not                | the free one adopts the associated one's channel  |
| Neither associated                     | fixed rendezvous channel (default 1, configurable)|
| Different Wi-Fi networks (diff channel) | can't ESP-NOW while both stay associated → **fall back to BLE control link**, or offer to move one device |

The ring's Wi-Fi recon scans hop channels and can miss ESP-NOW frames during a
scan — so the link layer tolerates loss (sequence numbers, retries) and features
that need the link schedule around scans rather than assume a live pipe.

## 5. Wi-Fi credential sharing

Pair once, and the Watch can provision the ring's Wi-Fi — the Watch has the
bigger UI for entering credentials, and joining the same network also aligns the
ESP-NOW channel *and* gives the ring SNTP/time:

- A `WIFI_PROV(ssid, psk)` `wh-link` command, sent **only over the established
  encrypted + authenticated session** (never in the beacon, never unencrypted).
  The ring stores it in NVS and joins.
- Direction defaults Watch→Ring but works either way (whoever has credentials
  shares). The PSK is protected by the SAS-derived session keys — sharing a
  passphrase over an unauthenticated channel would be an own-goal for a privacy
  tool, so it is gated behind pairing.

## 6. Built vs. next

Already in the `wh-link` library (tested): framing, ChaCha20-Poly1305 AEAD, SAS
pairing, `ANNOUNCE`/`CAPS`/`PING`, `DEVICE_SEEN` (+ the sec/freq extension),
`GPS_FIX`, `TIME_SYNC`, the LoRa bridge, `LOG_WRITE`.

New, small protocol additions: a `WIFI_PROV` command + a couple of `ANNOUNCE`
beacon fields (channel, pairing-mode flag). Everything else is Watch-side
platform glue: the BLE beacon advertiser/scanner, the ESP-NOW transport shim, the
channel coordinator, the rotating-pseudonym rendezvous, and the `WIFI_PROV`
handler. Built and host-simulated before on-hardware pairing (ring firmware isn't
stable yet).
