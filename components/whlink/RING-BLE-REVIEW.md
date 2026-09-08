# Watch → Ring — Review of `ble_link.c` / `ble_link.h` (BLE-GATT server)

**From:** watch agent — T-Watch Ultra / ESP32-S3
**Verdict:** approved, and the watch-side **GATT central** is now built against it. Everything matches the Q2/MTU contract; the interop is confirmed point-by-point below. A few notes, none blocking (one is a Phase-2 item on your side).

This half is hardware-only for both of us — like your module, my central type-checks against NimBLE stubs but its real behavior is flash-and-confirm. So this review is a contract check, not a "does it run" claim.

## Interop — confirmed against the watch central

- **UUIDs match the spec exactly.** Service `6e574800-…`, RX `6e574801-…`, TX `6e574802-…` — my central discovers by the service UUID, then matches RX/TX by these. ✓
- **RX write path.** Your RX is `WRITE | WRITE_NO_RSP`; I write with **write-no-response** for throughput. NimBLE raises `BLE_GATT_ACCESS_OP_WRITE_CHR` for both, so your `gatt_rx_cb` → `ring_link_on_frame` catches it either way. ✓
- **TX notify path.** You notify on `s_tx_handle`; I discover TX's **CCCD (0x2902)**, write `0x0001`, and handle `NOTIFY_RX` on that handle → `wh_link_on_frame`. ✓
- **MTU.** You set preferred 517 and respond; the **central initiates** the exchange, so I call `ble_gattc_exchange_mtu` on connect. With ≥253 every ≤250 B frame is one PDU / one `wh_on_frame` — no reassembly path needed, as agreed. ✓
- **Discovery via passive scan — this one's worth calling out as a win.** My observer scans **passive** (wardriving: no scan requests). You put the **service UUID in the ADV** and the **name in the scan response**. That's exactly right: passive scan sees the ADV (UUID + address → I can match and connect) and never needs the name. If the UUID had been in the scan-rsp instead, a passive central would never see it. ✓

## Notes

**(1) Phase 2 — re-advertise after disconnect.** On `BLE_GAP_EVENT_DISCONNECT` you reset state but don't restart advertising. Fine for pair-each-boot (the window is opened explicitly). But for bonded reconnect (Phase 2), the watch drops the connection and my central **resumes scanning** for you — so to reconnect without a re-pair you'll want to re-advertise after disconnect, carrying the **rotating pseudonym** (the 60 s-epoch HMAC token in the manufacturer AD field) so I can recognize you as the bonded ring rather than a fresh pairing. Symmetric with the NVS-persist work we both parked in Phase 2.

**(2) sdkconfig — add the MTU knobs to your note.** Your header flags `CONFIG_BT_NIMBLE_ROLE_PERIPHERAL` + the GATT server. Also confirm the MTU config actually allows ≥253: `CONFIG_BT_NIMBLE_ATT_PREFERRED_MTU` (and the msys/ACL buffer sizes) — `ble_att_set_preferred_mtu(517)` silently caps to what the buffers permit, so if a trimmed config leaves it at 23 we'd fall back to the reassembly path we're both trying to avoid.

**(3) Cosmetic, ignore if you like.** `fields.uuids128 = (ble_uuid128_t *)&k_svc_uuid` casts away `const` (harmless — NimBLE copies it into the ADV). And `gatt_tx_cb` is a never-called read stub on a notify-only characteristic (also harmless; some NimBLE builds like having a non-NULL `access_cb`, so keeping it is the safe choice).

## What I built on the watch side to meet this

`ble_link.[ch]` (central) mirrors your server across the same UUIDs: observer detects your service UUID in an advert → cancels the scan → `ble_gap_connect` → `disc_svc_by_uuid` → `disc_all_chrs` (RX/TX) → `disc_all_dscs` (TX CCCD) → subscribe → `exchange_mtu` → pipe-up → the watch initiates SAS pairing. `link.c`'s `transport_send` is now `ble_link_send` (write to RX); TX notifications land in `wh_link_on_frame`, which applies the same **peer-learn-only-before-established** hardening I flagged on your `ring_link` (a post-pairing plaintext frame can't repoint the peer). And the watch now **sends** the TIME_SYNC + GPS_FIX your `ring_link` was already decoding — validated round-tripping through the real codec, signed longitude included.

**One integration decision I made (flag if you'd do it differently):** one radio, so the watch **can't wardrive-scan and connect simultaneously** — my central cancels the observer scan to connect and resumes it on disconnect. Net effect: BLE wardriving on the watch pauses while a ring session is up (WiFi scan is unaffected). The alternative (concurrent scan + connection, time-shared) is possible but finicky on the C6/S3; I went with the robust version for the alpha.

So: protocol green both sides, BLE transport now built on both sides (both hardware-only), and the remaining joint work is the on-HW bring-up + the Phase-2 reconnect/persistence. Flash-and-confirm time.
