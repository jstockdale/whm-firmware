# WHM - Prebuilt Flash Package
version: v0.62.0

Waveshare ESP32-S3-RGB-Matrix (N32R16) + P2.5 64x64 1/32 HUB75E.
ESP-IDF v5.5.2, fullclean, gate-verified (diags=0, whlink PASS).

## Flash it

    bash flash.sh          # autodetects the port; BOOT-hold if sync fails

Writes ONLY: bootloader @0x0, partition-table @0x8000, otadata
@0x10000, app @0x20000. **NVS is never touched: your node's
identity keys, TOFU pins, WiFi credentials, and settings all
survive every update.** Peers will keep trusting this node across
releases - no re-pinning ritual.

## Factory reset (the ONLY time identity changes)

    esptool.py erase_flash     # wipes EVERYTHING incl. identity

After a factory erase the node mints a NEW identity; every peer
will then print `identity: MISMATCH ... REJECTED` - that is TOFU
doing its job. Run `keys forget <name>` on each peer exactly once
to accept the reborn node. If you didn't erase and see MISMATCH,
treat it as an alarm, not a chore.

## First boot / quick start

    wifi join YourSSID YourPass
    tz PST8PDT,M3.2.0,M11.1.0
    status                    # sync, identity fp, seal, audio, heap

Fleet: nodes elect an anchor automatically (`sync` shows status;
`sync help` for options). Music: `mp3 fleet play <n>`. Media:
`media play <f.whm>` / browser live-cast at `http://<node>/live`.
Diagnostics: 1Hz `[A]` ledger during fleet audio (`mp3 diag off`).

Panel power from its own 5V supply, never the logic board's USB.
