# SSH on the WHM panels (whssh)

Ported whole from the Whitehat Watch's wh-console-ssh r1. Public-key auth
ONLY (passwords refused in the auth callback), one session at a time,
per-device host key minted from the hardware RNG on first boot and stored
in NVS - fail-closed: no shared-key fallback, ever.

## First key (over USB, once per client key)
    ssh addkey ssh-ed25519 AAAAC3Nza...your-pubkey-b64...
Then from your machine (any username):
    ssh whm@<panel-ip>
`ssh keys` lists, `ssh rmkey <n>` removes, `ssh newkey` remints the host
key (reboot applies). The panels mount no SD, so NVS is the sole store -
the watch's SD paths fail cleanly into the NVS fallback by design.

## What you get on connect
The ENTIRE fleet console, verbatim: help, fleet, version, wifi, show,
keys, tz - every esp_console verb. The bridge is per-task stdio: the SSH
session fopencookie()s its own stdout onto the wolfSSH channel and runs
your line through esp_console_run(), so command output flows to you while
the USB REPL (its own task, its own stdout) is completely untouched.
Plus the LOG TEE: ESP_LOG lines stream to the session live (colourised,
CRLF-safe), while USB keeps its full feed - `log debug` works remotely.
Line editing: history, arrows, ^A/^E/^W/^U/^K, ^L - the watch's editor.
`exit` leaves.

## PSRAM placement (the watch's law, applied)
- wolfSSL/wolfSSH crypto arena: 128 KiB private multi_heap in PSRAM,
  mutex-guarded, ALL wolf allocation routed via wolfSSL_SetAllocators;
  fallback chain arena -> general PSRAM -> internal. Condition B
  (WOLFSSL_TRACK_MEMORY) is set GLOBALLY in the top CMakeLists so the
  managed components honor the allocators - without it the arena is
  dead code.
- authorized_keys table, log-tee ring: EXT_RAM_BSS_ATTR (task-only).
- per-session line editor (~4.9 KB): heap_caps SPIRAM, internal fallback.
- host key + 4 KB scratch: INTERNAL (small, boot-touched) - per the law.
- [VERIFY-HW] Condition A (register-mode, non-DMA S3 crypto) is the
  Espressif port's documented behavior; confirm on first on-device
  handshake, and if DMA crypto is ever enabled these buffers must move.

## Licensing
wolfSSL + wolfSSH are GPLv2 (or commercial). Same terms the watch builds
under; noted for THIRD_PARTY.md.
