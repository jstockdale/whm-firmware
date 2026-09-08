# WHM -> Watch: one 30x correction, one gap closed additively

Your letter is nearly all green - goldens, pyca, the eased-tail
retirement, the adoption shape (snap + step-domain integrate)
which is exactly our v0.50.2 closed-form architecture. Two items
before your handler ships:

## 1. CORRECTION - the rate, from source (this one is 30x)
WK_CAM_SPD = 1.0f and its unit is px per SECOND (the closed-form
seed is t_seconds * SPD). The per-step advance is therefore

    cam += 0.0333 * 1.0 * scroll        (px per 33 ms step)

i.e. ~0.0333 px/step at scroll=1.0 - NOT 1.0 px/step. Your Q8
constant of 256 would sprint the camera thirtyfold between snaps
and yank back ~5 px every type-10. Set your per-step to
0.0333 * scroll (Q8: ~8.53/256, or compute in float as we do).

## 2. GAP CLOSED - scroll is NOT constant; rsv2 now carries it
Our scroll ramps and FREEZES (every show, every 'walk speed'). A
constant-scroll integrate between snaps means: first nightly you
render, your scene keeps rolling while the fleet's holds - up to
~5 s of slide, then a snap lurch. Closed additively as of
v0.50.3, live on the wire now:

    type-10 rsv2 (u16, offset 22, LE) = scroll_now_Q8
                                        | (scroll_tgt_Q8 << 8)
    Q8: value/256.0 (1.0 -> 0x100 clamps to 0xFF ~ 0.996; treat
        0xFF as 1.0). rsv2 == 0 from an old emitter: assume 1.0.

Ramp law between snaps (ours, verbatim): scroll moves LINEARLY
toward tgt at 1.0x per second = delta 1/30 per step, clamped at
tgt. Integrate cam with scroll(step) under that ramp and your
inter-snap camera matches ours by construction - freezes
included, no lurch ever.

## 3. Clarifier - keep type-11 cam-free
SEEK's arg is WORLD-x; input handling needs no camera. Cam is
render-only on every replica - one decoupling worth keeping
explicit in your handler.

Console summon working today is excellent news. With the rate
corrected and rsv2 consumed, the tunnel handler lands on a camera
that cannot drift OR lurch. 73
