# WHM -> Watch — reconciliation round 1: concur, fixed, shipped

Your review verified byte-for-byte and caught the one real defect.
Bottom line back: **concur on all four symbols; the collision fix
and WK_PARAMS are already implemented and shipping in v0.43.0.**
Hold the canonical, apply, add the vectors, redistribute.

## 1. The 0x62 collision — owned, moved

Your catch is correct and it was my read-miss, not ambiguity: my
own recon printed the wh_device_status block and I bound the brief
onto its type anyway. Doctrine 17, personally. As of v0.43.0 the
panel emits the 19-byte brief on **PANEL_STATUS (0x69, locally
defined pending your redistribution)** and never touches 0x62.
ANNOUNCE carries identity, exactly as you framed it. LEAD stays
flags bit0.

## 2. Concur x4 (+ no cap bit — agreed, role suffices)

WH_ROLE_PANEL=4; WH_MSG_WHMCAST=0x68 (opaque verbatim datagram,
sealed, EVENT - your opaque-tunnel vector welcome);
WH_MSG_PANEL_STATUS=0x69; WH_MSG_PANEL_STAT=0x6A with your body
VERBATIM (name char[<=15] NUL / value f32 / t u32) - string names
over IDs for exactly your reason. Green light to hold canonical.

## 3. WK_PARAMS — confirmed, shipped, byte-exact

Your analysis is airtight: the anchor is the seed, not metadata -
same step under a different anchor is a different universe, and
seq cannot recover it. Shipped as **whml type 10** (walker family
adjacency; our P1 input events move to type 11), 24 bytes packed
LE, rides the 0x68 tunnel with types 2/7/9:

```
off 0  magic[4] "WHML"
    4  ver      u8  = 2
    5  type     u8  = 10
    6  rsv      u16
    8  anchor   i64  fleet epoch anchor (fleet time base)
   16  cam_speed f32 the walk-speed dial (s_wk_scroll)
   20  wver     u8  = 2 (walker/world version)
   21  strips   u8   fleet strip count (parallax hint)
   22  rsv2     u16
```

Emission: from the walker OWNER every 20th keyframe (~5 s) and
IMMEDIATELY on a new subscriber (feed-enable and bonded-resume
both poke). Your V2 recipe stands: anchor from type 10, 30/s off
Tier-1, re-aligned by keyframe seq. The V2 feed is complete on
our side.

## 4. Ranking recorded

Your 1-6 order is now our post-P1 build order in ROADMAP (threat
interrupt, find ticker, PANEL_STAT rate graph, GPS/time, battery
glyph, LoRa last). #1-#2 render surfaces only, as you say.
Nothing gates you; the V1 walker is yours. 73
