# Security model - the Signet and the Seal

## Threat model
LAN-local attacker who can sniff, inject, and replay UDP broadcasts.
Goals: no unauthorized fleet commands; no forged walker/audio state;
key material never in the clear. Out of scope: WiFi/WPA2 compromise,
physical/serial access (the console is root by design).

## Identity - the Signet (0.51.0)
Each node mints Ed25519 (sign) + X25519 (exchange) keypairs on first
boot; persisted via the NVS broker (hex-encoded; see the 0.55.x saga in
git log for why the broker's front door matters). Public halves ride
type-13 IDENT beacons (~15s). **TOFU** exactly like SSH: first-seen
pinned with a printed fingerprint; any later mismatch is screamed and
rejected, never silently re-pinned; `keys forget <name>` is the only
door. Announces themselves are unsigned by design - TOFU *is* the
bootstrap; spoofing one buys peer-table cosmetics, nothing more.

## Commands (control plane)
`[payload 212][nonce8 = issuer TSF][Ed25519 sig64]` = 284B, same type
byte (receivers distinguish by size). Verified against the pinned key
of the embedded issuer name; per-issuer monotonic nonces in a +-10s
window (equal nonce = our own x3 burst, silently deduped). Unsigned
212B frames ride a loud one-release legacy grace.

## State plane - the Seal (0.52.0)
Anchor mints a random 32B fleet key; distributes per pinned peer as
type-14 KEYWRAP: `crypto_aead_lock` under the pairwise X25519 shared
secret, fresh 24B nonce, rebroadcast until keyed. Types 8/9/10/11 then
carry a 16B keyed-BLAKE2b tag (microseconds each; 30Hz is free).
Replay economics ride the payloads' own monotonic fields (step / tsf).
`secure strict on` (NVS) turns grace warnings into drops.

## Operations
`keys` (my fp, pk, pins, forget) - `secure` (key present, strict) -
`status` ident row (fp | pins N | seal keyed/STRICT).

## Known residuals
- `keys rotate` not yet a command (fleet key re-mints each boot).
- Broker staging key field is 16B: node names must stay <= 10 chars.
- Crypto: Monocypher 4.0.2 vendored, SHA-256-pinned in the 0.51.0
  commit message.
