# SPEC — the WHM walker, portable (for the watch agent)

The walker is a DETERMINISTIC REPLICA by construction (WHM
doctrines 8, 9, 13-15): every renderer that shares the anchor, the
step clock, and the pure RNG computes the identical world and
identical pose with no communication; keyframes exist only to
verify and to carry owner intent. That makes a watch port a
first-class citizen, not a mirror hack. Authority: main/ui.c in
the WHM repo (wk_* functions) is the reference implementation;
this document is the orientation + wire contract.

## 1. The clocks

- Step tick: WK_TICK_US = 33000 (30 steps/s).
- Anchor epoch: WK_ANCHOR_US = 600000000 (10 min). anchor =
  tsf - (tsf mod 600e6); rollover = scripted respawn (a replay
  point, not an error). step = (tsf - anchor) / 33000.
- On WHM, tsf = the WiFi TSF (us). On the watch: Tier-1 wall time
  from wh-link TIMESYNC (ms-class). Step-indexing absorbs ms skew
  as sub-pixel phase - acceptable on a separate screen by design.

## 2. The pure RNG (the load-bearing law)

Never stateful. Every draw is f(anchor, step, draw#):

```c
draws = 0 at each step entry;
r = wk_h( (uint32_t)(anchor & 0xffffffff)
        ^ steps * 2654435761u
        ^ (++draws) * 0x9E3779B9u );
/* wk_h = lowbias32: v^=v>>16; v*=0x7feb352d;
          v^=v>>15; v*=0x846ca68b; v^=v>>16; */
```

Draw ORDER inside a step is part of the spec: consume draws in the
same sequence as wk_step() or you fork the universe (doctrine 12's
cousin). Port wk_step verbatim rather than re-deriving.

## 3. World + camera

- Ground line y=57 (WK_GROUND); reach 21 (WK_REACH); camera speed
  1.0 px/step (WK_CAM_SPD), owner-adjustable via `walk speed`.
- Camera law: screen_x = world_x - (k*cam + 64*strip_idx). k=1 for
  the stage, k<1 for parallax layers. A single-screen watch is
  strip_idx 0 with its own viewport width.
- Terrain/rewards are chunk-generated from the pure RNG (64-px
  chunks); chunk entry marks are step-time events (doctrine 13).
  Reference: wk_chunk/wk_flora/wk_step in ui.c.

## 4. Wire — type-9 keyframe, 52 bytes packed LE

```
off 0  magic[4]           (whmcast family magic - see sync.c)
    4  ver      u8  = 2
    5  type     u8  = 9
    6  owner    u8   strip idx currently simulating
    7  st       u8   state machine id
    8  dir      i8   facing
    9  y        i8   pose y
   10  timer    u16  state timer
   12  seq      u32  THE FUTURE STEP this keyframe describes
   16  x        f32  world-x
   20  from[16]      sender node name
   36  tsf      i64  owner's send-time tsf (ownership ranking)
   44  tgt      f32  current target-x   } complete-promise
   48  vx       f32  current velocity-x } fields (doctrine 14)
```

Owner emits ~4 Hz at 3x sync-lead into the future (shadow sim).

## 5. Replica contract (doctrines 13-15, the short form)

- Buffer future refs keyed by seq(step); verify AT that step with
  zero evidence age; match = consume silently.
- Mismatch = COMPLETE-STATE SNAP to the keyframe (all fields incl
  tgt, vx) at exactly that step. NEVER nudge a deterministic
  replica.
- Snap storm (>5 in 3 s) = replay-resync: respawn from anchor and
  re-run to now. Provably exact.
- Entry to the walker screen = forced replay point.

## 6. Watch port, two modes

- V1 STANDALONE (zero dependencies): own anchor from local clock,
  render solo. Ships with just this spec + a wk_step translation.
- V2 FLEET VIEWER (after WHM L5): panel lead tunnels type-9 via
  WH_MSG_WHMCAST; watch runs the replica at Tier-1 clock and
  consumes keyframes per §5. Same walker, on the wrist.

Futures worth agreeing before either ships V2: a WK_PARAMS
descriptor (speed dial + anchor echo) so a joining viewer needs no
console query. Proposed as part of the WH_MSG_WHMCAST payload
discussion.
