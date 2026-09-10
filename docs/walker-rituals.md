# Walker rituals - the campfire and the hammock
## Feature specs of record, 2026-09-09. Both ideas: Robin.
## Grammar precedent: the little red chair (SETUP -> SIT -> PACK).

## 1. THE CAMPFIRE (night ritual)

WHEN: night only, by the FLEET's clock - dayclock f below
~0.12 (never a local clock). Occasional, chair-frequency
class; never twice in one night.

WHERE: he stages to the right of his current strip's action -
walks a little ahead, picks flat ground clear of ladders and
house doorways.

THE ARC (new states, u8 st has room):
- FIRE_BUILD: kneel; place a stone ring (3-4 px stones);
  stack wood (two crossed sticks); strike - a spark pixel,
  then flame catches and GROWS over ~2 s. Cute beat: he
  looks around for wood first (two short walk-offs and
  returns with stick sprites).
- FIRE_SIT: sits by the fire (chair pose family, no chair).
  Flame flickers (2-3 frame loop, hue amber->orange); a warm
  GLOW halo lights the ground and his front side at night -
  the panels will love this. MARSHMALLOWS (phase-driven,
  sometimes): stick out, white blob on tip, held over flame;
  blob turns golden then brown over ~4 s; he eats it (blob
  vanishes, tiny happy hop of the head). 1-3 marshmallows
  per sitting.
- FIRE_DOUSE: THE SACRED ORDERING - the fire goes out BEFORE
  he ever walks off-scene. He produces the bucket (same prop
  economy as the chair), tips it: 3-4 water pixels arc onto
  the flame; flame collapses; a STEAM/SMOKE puff (3 gray
  pixels rising, fading); embers dim to dark stones. He
  stows the bucket, stands.
- Resume walkabout.

DURATION: build ~4 s, sit 20-60 s (marshmallow beats
inside), douse ~3 s.

WIRE: none new. States + phase + timer in the existing kf
carry everything; the fire anchors at a fixed offset from
his BUILD x (chair trick). Observers (panels, monitors,
viewer) each add the fire/glow/marshmallow drawing to their
sprite layer. Rung purity: rituals are pose-layer; sim
determinism untouched (ritual choice seeded from step +
chunk id like all walker whims).

## 2. THE HAMMOCK (Robin's, day or night)

WHEN: any hour; occasional. Requires the ENVIRONMENT to
allow it.

ANCHORS - the structural set, NEVER platforms:
ladders, trees (oak or pine trunks), house walls. Any pair
with horizontal spacing ~8-16 world px qualifies. Discovery
is DETERMINISTIC: ladders and houses live in chunk structs,
trees are pure f(chunk) flora - every unit and every
observer computes the same candidate list from cam
neighborhood. No wire for discovery.

HEIGHT + REACHABILITY (v1 rule): the hammock hangs at the
lower anchor's usable height. Ground-level pairs (two
trees): sling at ~3 px up - step in. Elevated (ladder-to-
tree, ladder-to-ladder): sling at a rung height the walker
can reach by his normal climb - v1 requires at least one
anchor be a ladder he can climb, and he enters from that
ladder. High is fine when reachable; that IS the charm.

THE ARC (new states):
- HAM_SETUP: at anchor A - rope-throw animation (arc of 2-3
  rope pixels to anchor B), tie-off wiggle at each end,
  fabric UNFURLS (a curved 6-8 px sling drops into its
  catenary over ~1 s).
- HAM_IN, phase-driven modes: SWAY (gentle sinusoid of the
  sling + him, ~0.3 Hz), NAP (eyes-closed stillness, slower
  sway, maybe a tiny 'z' pixel that floats up and fades,
  every few seconds), WATCH (head/dir flips now and then,
  following nothing in particular). 30-90 s.
- HAM_DOWN: he rolls/steps out, unties (reverse wiggles),
  the fabric rolls up into a carried bundle that vanishes
  (prop economy), resumes.

WIRE: none new beyond states. Anchor pair is re-derivable:
observers recompute candidates near his SETUP x and pick by
the same seeded rule - byte-free agreement, same philosophy
as terrain.

## Sequencing
After the current monitor/Pro/LoRa arc. Campfire first
(night glow + marshmallows = highest cuteness-per-line),
hammock second. Each ships fleet + monitor + viewer sprite
layers together, one release, verify-on-glass card each.
