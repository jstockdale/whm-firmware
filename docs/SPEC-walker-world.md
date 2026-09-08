# SPEC — the WHM walker WORLD (for the watch agent)

Companion to SPEC-walker-port.md: the port spec gave the ENGINE
(clocks, RNG, wire, replica contract); this gives the UNIVERSE -
every generator's seeding and draw order, the state machine, the
physics, the layers, the art inventory, and the special modes.
Authority stays code-first: pixel art ports VERBATIM from the
named functions in main/ui.c; this document carries the seeds,
orders, gates, and invariants byte-exact, because one draw out of
sequence renders a different planet.

## 1. The two randomness domains (the load-bearing split)

- **Terrain is ETERNAL: pure f(chunk_id), anchor-FREE.** Chunks
  hash from their id alone, so the land survives anchor rollovers
  and needs nothing from WK_PARAMS. (Corrects the port spec's
  looser wording: only STEP-domain randomness seeds on anchor.)
- **Behavior is EPOCHAL: pure f(anchor, step, draw#)** via wk_rnd
  (port spec §2). Autonomy gates, ritual rolls, fireworks - all
  step-domain. Draw ORDER inside a step is part of the spec.
- **Rewards are SESSION state**: seen[] visit marks (WK_SEEN_N=48)
  accumulate from chunk entries and feature uses, decay the
  reward tables, and are rebuilt identically by replay (marks are
  step-time events, doctrine 13). A viewer reproduces them by
  running the same replica; they are never on the wire.

## 2. Chunk generator (wk_chunk) — byte-exact

Chunk = 64 px, base = id*64. Cache 6 slots by id%6. Hash chain:

```
r0 = wk_h(id * 2654435761 + 0xB16B00B5)
np = 3 + (r0 % 3)                          /* 3-5 platforms */
per platform k (in order, chained):
  r = wk_h(r)
  w = 14 + (r % 20)                        /* 14..33 */
  x = base + wk_h(r+7)  % (64 - w)
  y = 12   + wk_h(r+13) % 34               /* 12..45; ground=57 */
```

Ground gap (independent stream):
```
gr = wk_h(id * 977 + 5)
exists iff (gr & 3) == 0                   /* 25% of chunks */
w = 10 + ((gr >> 4) % 5)                   /* 10..14 */
x = base + 8 + ((gr >> 8) % (48 - w))
bridged = ((gr >> 2) % 5) < 3              /* 60% bridged */
```

House (independent stream):
```
hr = wk_h(id * 31337)
present iff (hr & 3) == 1                  /* 25% */
x = base + 4 + ((hr >> 4) % 44)
SUPPRESSED if [x, x+14] overlaps gap span +-2 (no houses over pits)
```

Ladders (<=3) + slides: per platform, find the best platform
strictly below with x-overlap (else the ground); if the drop
exceeds WK_REACH(21), place a ladder whose x-range is the
INTERSECTION of both spans - ladder FEET always rest on something.
Slides mirror. Port wk_chunk verbatim for the tail (slide gen +
exact ladder emit); the invariants above are normative.

**Traversability invariant**: every gap is bridged or long-
jumpable; the walker can always continue. NYE-window chunks
(wk_nye_window) generate EMPTY (open plaza); wk_nye_scene draws
poles + lights there.

## 3. Rewards + autonomy gates (WK_WALK, in draw order)

Visit decay tables (index = wk_visits(key), clamped):
- platforms  key = chunk*8 + k : { 1.0, 0.15, 0.05, 0.02 }
- ladders    key = 0x40000000 ^ x : { 1.0, 0.7, 0.5 }
- slides     key = 0x20000000 ^ x : { 1.0, 0.7, 0.5 }

Per WALK step (each `wk_rnd()` consumes a draw - order sacred):
1. open-gap lip ahead (+2 px dir) at ground -> WK_CROUCH timer=8
   (the charged long-jump; deterministic, no roll)
2. slide here:  r%100 < fun*45  -> WK_SLIDE (mark)
3. ladder up:   r%100 < fun*60  -> WK_LADDER up (mark, fresh)
4. ladder down: r%100 < fun*30  -> WK_LADDER down (mark)
5. platform edge within 1 px, higher: gate = rw*90 + 8;
   dh<=9  -> WK_JUMP hop (vy=-1.6, vx=dir*0.45)  (mark)
   dh<=21 -> WK_CLIMB tgt=y                       (mark)
   else if !turn_cd -> consider turn
6. no support underfoot -> WK_FALL
7. leash: |x - (cam + idx*32 + 32)| > 60 walking AWAY ->
   dir flips, turn_cd = 50 (once, latched)
8. scout (wander bias): +-2 chunks, features 3..90 px ahead,
   score += reward / (1 + dist/24); s_wk_pure forces 0
   (the `walk pure` bisection switch)

## 4. State machine (13 states) + physics

WALK, CLIMB, LADDER, SLIDE, FALL, IDLE, CROUCH, JUMP, LAND,
GOCHAIR, SETUP, SIT, PACK (the camping quartet).

- gravity: vy += 0.2 per step (JUMP/FALL)
- hop:     vy = -1.6, vx = dir*0.45
- CHARGED long-jump: CROUCH winds up timer=8, then vy = -1.9
- ladder top-exit hop: vy = -1.4
- LAND -> WALK, vx = vy = 0
- P1 USER mode (v0.44.0): overrides inside WALK - instant
  heel-turn, stop holds ground, jump = the charged windup;
  autonomy draws SKIPPED while USER (identical on every replica
  because the input log is shared); 300-step shake-off.

## 5. Layers + draw order (pat_walker, painter's)

screen_x = world_x - (k*cam + 64*idx) for every layer (seam-
continuous; lroundf like the platforms - one shared beat):

1. wk_sky   - gradient by day-factor f; sun (r11, r15 at dusk;
   LIVING RIM hot-spot ~8 s/rev + breathing glow + 2 px ORBITING
   RAY) / moon (r9, same dimmer, traced on the crescent) on the
   time-true solar arc at k=0.04; star field; three cloud species
   (wisp / double-puff / 24 px shaded cumulus) bobbing on own
   phases, each wearing TWO running lights (bright 3 px top edge
   L->R, dim 5-6 px bottom counter-current R->L); wk_comet inside
   (see §7).
2. wk_flora - k=0.7; chunks c0-1..c0+2 of the FLORA base; skipped
   near NYE windows; night light li = 0.35 + 0.65*f; species per
   chunk hash (port wk_flora verbatim): large oaks 28-34 px
   (ragged three-shade canopies) / layered pines 26-38 px on ~30%
   of chunks, mediums ~25%, old tiny kinds as dimmed accents;
   synthwave silhouettes via sw.
3. wk_birds - day: 5 px gull, two-frame V-BEAT (shallow-V glide /
   deep-V flap, tips up a full row; one vertex is a bird) at star
   parallax; small hours: the rare synthwave pigeon (magenta
   underlight, cyan wing-V).
4. The STAGE (k=1): ground line y=57, platforms + shading, houses
   (windows), ladders, slides, gap lips + bridges.
5. The walker (sprite frames per state incl the scarf; port the
   draw functions verbatim).
6. Overlays (fireworks, NYE scene, name nods).

## 6. Special modes

- NIGHTLY fleet mini-takeover: type-7, year=0; one initiator at
  its LOCAL 23:59:40 broadcasts; every replica runs the identical
  TSF-locked ~25 s script - GOCHAIR by ~2 s, SETUP, SIT, ten
  seconds of seeded fireworks at midnight, linger, PACK, release.
- NYE (real year): the long form - ritual begins ~170 s before
  midnight; plaza chunks empty via wk_nye_window; wk_nye_scene
  poles + string lights; the full show + music-egg hook.
- SYNTHWAVE HOUR: 03:00-05:00 LOCAL, 15-minute fades in/out -
  purple -> hot pink sky, cyan/magenta stars, the big sun in
  horizontal skip-lines; sw in [0..1] blends every layer.
- COMET REGISTRY (real-sky visitors): active only in true
  visibility windows - 2P/Encke Dec 2026-Feb 2027 (peak 01/25),
  C/2026 C1 Tsuchinshan Oct-Dec 2028, 46P/Wirtanen Sep-Dec 2029,
  103P/Hartley 2 Mar-May 2030. Two-tail pixel anatomy (shimmering
  dust + flickering cyan ion), triangular intensity ramp across
  the window, gold 3x5 name nod first 12 s of every 10th minute,
  k=0.06, behind clouds, dimmed by f<0.45. Updates by commit.
- USER control (P1), and `walk pure` (rewards off, bisection).
- Camping ritual (autonomous): occasional IDLE -> GOCHAIR chain
  even outside the nightly - he takes breaks.

## 7. Art inventory -> source map (port verbatim)

walker sprite/frames: wk_draw_* per state | terrain: the stage
block in pat_walker | flora species: wk_flora | clouds + lights:
wk_sky cloud section (CLD_EDGE / CLD_BOT) | sun/moon living rim:
wk_sky | gulls/pigeon: wk_birds | comet: wk_comet + s_comets[4] |
fireworks: the seeded burst fn in the ritual path | chair:
GOCHAIR/SETUP draw | NYE scene: wk_nye_scene | palettes: the
f/sw blends at each layer head. The 3x5 glyph font (A-Z 0-9) is
fw_font + fw_glyph.
