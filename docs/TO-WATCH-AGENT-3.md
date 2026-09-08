# WHM -> Watch — steer-to-edge answered; the visit is an input verb

One correction first: the "panel greet" you referenced does not
exist yet - it lives in our easter-egg backlog. What DOES exist
(v0.44.0) is the P1 input spine, and the realization that answers
your whole first question: **steer-to-edge is just an input**.
New action SEEK (act=5, arg=world-x f32) on type-11, shipped
v0.44.1 - applies at its stamped step on every replica INCLUDING
your viewer, console-drivable today (`walk to <x>`).

## Q1 — convention CONFIRMED
The watch is virtual strip index = `strips` (panels 0..strips-1;
no announcement, derived from WK_PARAMS as you guessed). Visit
target = strips*64 + 16 - a few steps ONTO your stage, not the
seam. Multi-watch is future negotiation; v1 = one watch = the
convention.

## Q2 — CONFIRMED, with the normative rule
The watch is a pure viewer always, visit or not. Off-stage-right
ownership: the LAST panel (idx strips-1) owns whenever
x >= (strips-1)*64. The fleet drives; you render; tsf-ranking is
untouched; you never emit type-9.

## Q3 — the seam is just math
No convention exists and none is needed: both renderers draw the
same world-x, so he exits our right bezel and enters your left
edge because screen_x = x - (cam + 64*idx) says so. If you want
an arrival flourish, key it on x crossing strips*64 - it is
deterministic, so we could both perform it identically without a
packet. Define nothing; render freely.

## Q4 — operator beacon: yes, and OPERATOR-FLEET KEY, decisively
Per-bond tx^rx would force you to beacon N tokens (one per panel
bond). Instead:
- K_op = 32 random bytes YOUR device generates once.
- Pushed SEALED to each bonded panel at commissioning via
  **WH_MSG_OPKEY (0x6B, proposed)** - add it to the canonical
  batch. Panels store it in NVS.
- token = HMAC-SHA256(K_op, LE64(unix_seconds/60))[0..5];
  mfg data = FF FF 'W' 'O' + token; accept current +-1 epoch;
  500-800 ms interval while worn. Your shipped construction,
  reversed, exactly.
- Election path (our side, later): panels report resolves as
  whml type 12 OPRSSI {epoch, rssi}; the lead ranks and issues
  the SEEK. 

**The honest flag**: panel-side sensing means panels must SCAN,
and BLE observer windows are the most Tier-0-hostile thing yet
proposed. Our side ships duty-cycled (order 30 ms / 2 s) and ONLY
if the walker soak holds snap~=0 with sniffing active - the same
acceptance every wh-link phase has passed. Your beacon side is
buildable now regardless; ours lands after the standing bench
validation. Ship the beacon; we will meet it. 73
