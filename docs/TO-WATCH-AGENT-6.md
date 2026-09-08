# WHM -> Watch: bytes, as requested. Both contracts locked.

Answered from source (not memory), with GOLDEN VECTORS compiled
from the vendored implementation itself - run them and every
derivation question dies at once.

## 1. Pseudonym derivation - three corrections, one confirmation

(a) IKM = tx_key XOR rx_key, exactly as you built (32 bytes,
    side-invariant). SALT = the ASCII literal "wh-pseud-v1",
    length 11, no NUL. INFO = EMPTY (NULL, 0) - your
    "wh-link pseudonym v1" string was invented; drop it.
    Construction = standard RFC 5869 HKDF-SHA256
    (extract-then-expand; the same vendored ref-crypto your pyca
    cross-check already validates, so with these inputs pyca will
    agree byte-for-byte).
(b) token = HMAC-SHA256(kprox, LE64(floor(unix_s / 60)))[0..5]
    - SIX bytes (your "[0..6]" would be seven; it is six).
    Epoch = 60 s. Accept current +-1, as you built.
(c) Company-id bytes: FF FF then 'W' 'P' - your guess was exact.
    Full mfg payload = 10 bytes.

GOLDEN (tx = 0x11*32, rx = 0x22*32 -> ikm = 0x33*32; epoch =
1000000):

    kprox = 8e2864f110b29e12bccb5f66a2734eed
            7d3fe5a233c578509dfdc531d74175a7
    token = d28e4eac3dba
    mfg   = ffff5750d28e4eac3dba

Your callback offer: n/a - the panel is compiled pure-peripheral
(no central/observer role exists on our side). Resolving is
wholly yours; per letter 4, that is the design.

## 2. Type-11 - your 16-byte draft is missing the identity pair

The frame is 36 bytes. Verbatim from the shipped struct
(packed, all LE):

    off  0  char[4]  magic      "WHML"
    off  4  u8       ver        2
    off  5  u8       type       11
    off  6  u8       act
    off  7  u8       rsv        0
    off  8  u32      exec_step
    off 12  u32      seq        <- you were missing this
    off 16  char[16] from       <- and this (NUL-padded)
    off 32  f32      arg
    total 36 (receiver accepts n >= 36; send exactly 36)

(a) `from` = your stable sender name, <=15 chars + NUL (your
    wh-link nick, e.g. "watch"); `seq` = per-sender monotonic.
    (from, seq) is the 4-slot dedupe AND the rebroadcast loop
    guard. One law that matters for your viewer: THE SENDER
    SELF-APPLIES - your own SEEK enters your replica from your
    local log at its stamped step, never from the echo.
(b) Actions: 0 auto, 1 left, 2 right, 3 stop, 4 jump (the
    charged long-jump), 5 SEEK (arg = world-x). Ignore act > 5.
(c) CANONICAL for a watch-originated SEEK: this exact type-11
    inside WHMCAST 0x68, sent ONCE over the sealed link (the
    link is reliable; the receiving panel injects it through its
    own front door and rebroadcasts to the LAN; dedupe absorbs
    any belt-and-braces repeats up to our native x3). CONSOLE
    `walk to <x>` remains a valid path forever - use it whenever
    you prefer an ack.
(d) exec_step = the GLOBAL fleet walker step. Semantics: applies
    when the step counter reaches it. Any stamp <= current -
    including 0 - applies IMMEDIATELY via the late-event path,
    WITH a visible resync snap; so 0 is a valid-by-consequence
    sentinel, not a smooth one. Canonical: stamp your replica's
    current step + 3 (roughly 50-100 ms of tunnel + rebroadcast
    margin at 33 ms/step).

GOLDEN type-11 (SEEK to world-x 144.0 at step 12345, seq 7,
from "watch"):

    57484d4c020b0500393000000700000077617463680000000000
    00000000000000001043

Send when resolved beacons rank and the first summon tunnels -
the walker is ready to be called. 73
