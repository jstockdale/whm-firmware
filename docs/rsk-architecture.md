# RSK - Replicated Sim Kernel
## Architecture for the WHM fleet library
### v0.1 draft - 2026-09-09 - John Stockdale & Claude
### (working name; bikeshed welcome - rsk_ prefix, components/rsk/)

## 0. Thesis

Extract laws, not code. Every defect in the 0.58-0.62 campaign was a
violation of a nameable law: hidden state outside the blob (Pure
Stream), partial snapshots (Whole Pose), two clocks (Right Clock),
frame-side integrators (One Camera), horizons that outrun their rings
(Near Horizon), wire gates that do not obey sizeof (the 24/40 relic),
unilateral epoch destruction (site 3496). The kernel's job is to make
each violation unrepresentable - by construction where possible, by
static_assert where not, by a gauge that confesses where neither.

The result: distributed apps on the fleet that are pixel-perfect
(deterministic replication) and robust (continuously audited, self
-healing, partition-tolerant on the render plane).

## 1. What tonight proved (the extraction inventory)

Field-verified in the 10-minute run and the overnight soak rig:

- Deterministic replication from (anchor, step, key) alone.
- Quiet Replay: fast-forward with wire silent and ownership frozen.
- Promise court: owner publishes clairvoyant frames; followers judge
  at equal step, 30 verdicts/s; materiality separates confession from
  silent healing; adopt-always caps fork life at one step.
- Oracle ladder: canonical-manifest hash every 32 steps; strict core
  + soft tag; O(1) divergence detection over ALL fields.
- Soft resync: epoch is fleet property; no replica may nuke it.
- Ledger gauges: the [W] line solved four roots in two captures.
  Observability is a core feature, not garnish.

## 2. Layer model

L0 TIME       TSF discipline, anchor windows, epoch turns, UTC stamps
L1 IDENTITY   node id, priority, election, SEAL keywrap, membership
L2 COMMANDS   whmcast deadline-exec (already app-agnostic)
L3 SIM KERNEL the new extraction (this document's core)
L4 APPS       walker (client 1), media-sync (2), fireworks (3)

Rule of three satisfied before extraction begins: three L4 clients
exist or are scheduled. Extraction is mechanical porting, not
speculation.

## 3. L3 core features

### 3.1 The Blob Law
Registered state is ONE POD struct: memcpy-able, statics forbidden,
no pointers, no heap. RSK_STATE(type) wraps registration with
static_asserts (size cap, trivially-copyable). The shadow emitter,
promise restore, oracle hash and future save/load all become legal
by this single constraint. A statics-audit script (tools/) greps the
app translation unit for state leaks before every release.

### 3.2 The Manifest (single source of truth)
RSK_FIELD(name, class) declarations generate, from ONE list:
- the canonical hash walk (strict vs soft class) - no padding ever
- the keyframe wire struct + pack/unpack
- the promise-restore assignments
- the materiality default table (app may override per field)
- the sizeof-only rx gates
One manifest feeding hash, wire and judge means they cannot drift
apart - the 24/40 relic and the yq1 quantization feedback both grew
in gaps between hand-written copies of the same knowledge.

### 3.3 Pure step contract
step_fn(state*, step_no, const inputs*) with:
- rsk_rnd(key): hash(anchor, step, key) - the only randomness
- no clock reads, no wire, no globals (audited)
- inputs only via the stamped input log (apply-at-step)

### 3.4 Scheduler
want = f(anchor, tsf_now); replay flag = (want - steps) > 3; during
replay the book is read-only and the wire is silent (Quiet law).
Epoch turn order fixed: steps=0 -> derived-state origins -> respawn
hooks (Ordered Epoch). Camera-class derived state must be pure
f(anchor, step) - integrator+rewind designs are rejected in review.

### 3.5 Authority module (optional per domain)
Ownership with: handoff hooks (Edge Law), adoption, grace,
demoted seize (live-only + true silence + geometric predicate),
and ONLY soft resync - the kernel does not export an anchor
-invalidate for replicas. Multiple independent authority domains
per sim allowed (walker strips are one domain; a future two-sprite
app is two).

### 3.6 Promise court
Shadow-run emitter (save/restore whole blob), horizon K with
static_assert(K <= RING - 2), promise ring keyed by step, at-step
judge, materiality predicate, adopt-always full restore with
sub-quantization guards on lossy fields, storm counter -> soft
resync. Counters standardized: ok/snap/stale/wm/ms/pr/pn.

### 3.7 Oracle
Rung cadence config (default 32 steps), conductor publishes,
followers judge banked-vs-cached at equal step, epoch-keyed,
replay-silent, confession prints the manifest. or= gauge.

### 3.8 Gauges
Auto-generated 1 Hz ledger line per registered sim, diag toggle,
UTC-stamped. Format stable so tools/whm_oracle_diff.py generalizes.

### 3.9 Wire discipline
Version byte per type; gates generated from sizeof; seal integrated;
new types allocated in one registry header. Budget note: keyframes
are owner-broadcast, oracle is conductor-broadcast - both O(1) in
fleet size n. Current spend ~2.5 KB/s total; a 6-panel fleet costs
the same wire as 2.

### 3.10 Host harness
step_fn compiles on host (pure by contract): golden-hash regression
per release, fuzzers for handoff/replay orderings, ledger differ.
CI gate alongside components/whlink/verify.sh.

## 4. Extras - the honest verdicts

Evaluation frame for THIS fleet: 2-6 nodes, one AP, art workload,
derived (regenerable) render state, humans present, restart cheap.
Each feature judged by: failure prevented / our exposure / cost.

### 4.1 Leadership election
Verdict: CORE (already shipped). Priority-based (bully-family) is
correct for a fixed, trusted, small roster; Raft-style randomized
terms buy zero here. Upgrade path is failure-detector quality
(silence thresholds we already tuned for seize), not protocol swap.

### 4.2 Quorum consensus (Raft/Paxos) on the SIM plane
Verdict: NO - and this is the pushback. "Synchronized" and
"consistent" are different goals. Consensus buys safety of durable
writes under partition by REFUSING service without majority. Our
render state is derived from time and regenerable; a partition (AP
flap) should leave BOTH panels performing - self-anchored, healed by
rank on rejoin - which is exactly what the current design does. A
quorum sim plane would freeze panels during every AP hiccup: strictly
worse art in exchange for a guarantee our workload does not need.
Also mechanical: n=2 has no meaningful majority. We are deliberately
AP-in-CAP on the render plane; convergence, not agreement.

### 4.3 Shared state broker (the CONTROL plane) 
Verdict: YES - this is where your instinct lands, and where real
agreement pays. Some state is NOT derived and must not fork: fleet
key epoch, playlist/schedule versions, per-node config, named
ownerships. Design: a small replicated key store
(key -> value, ver, writer_prio, epoch), writes serialized through
the conductor, last-writer-wins by (epoch, ver, prio), gossip
anti-entropy on rejoin, and an oracle-style hash of the whole config
space so drift confesses within a second. NVS-backed, tiny, restart
-safe. If the fleet ever grows to >= 3 always-on nodes AND a write
appears whose loss is unacceptable, upgrade that one keyspace to
single-decree viewstamped writes over the same conductor - the
broker API does not change.

### 4.4 Voting
Verdict: YES as a primitive, NO as a protocol. Real in-scope uses:
sensor fusion (n IMUs vote "shelf touched"), render sanity (panel
framebuffer CRCs vote out a glitcher, which then soft-resyncs),
audience/show choices. Implementation is nearly free: values ride
the existing deadline-exec plane, tally is deterministic at the
deadline on every node. rsk_vote(topic, value, weight, deadline).

### 4.5 Byzantine tolerance
Verdict: NO. SEAL already restricts the wire to authenticated peers;
BFT machinery for two trusted chips is pure cost. Revisit only if
untrusted third-party nodes ever join the piconet.

### 4.6 CRDTs for soft state
Verdict: OPTIONAL, elegant. The visits/seen class ("soft" tier) maps
cleanly to G-set / PN-counter merges on rejoin instead of heal-per
-window. Small win, zero risk, nice paper-trail. Slot after M3.

### 4.7 Decision matrix

| Feature            | Failure prevented        | Exposure | Cost | Verdict |
|--------------------|--------------------------|----------|------|---------|
| Prio election      | leaderless fleet         | daily    | done | CORE    |
| Failure detector   | flappy seize/handoff     | seen     | low  | CORE    |
| Sim-plane quorum   | forked durable writes    | none*    | high | NO      |
| Config broker      | forked config/keys       | real     | low  | M3      |
| Broker->VS writes  | lost critical write      | rare     | med  | LATER   |
| Vote primitive     | ad-hoc fusion bugs       | soon     | low  | M5      |
| BFT                | malicious insider        | none     | high | NO      |
| CRDT soft tier     | rejoin heal gaps         | minor    | low  | OPT     |
(*render state is derived; forks cost seconds of art, self-heal.)

## 5. Milestones

M1  Statics audit tool + Blob Law asserts; walker state formally
    registered (no behavior change).
M2  Manifest codegen: hash + wire + restore from one list; oracle
    and keyframes regenerated; golden-hash CI.
M3  Config broker (NVS-backed, conductor-serialized, hash-audited).
M4  Media-sync ported as client 2 (proves kernel generality).
M5  rsk_vote primitive on the command plane.
M6  Host harness + fuzzers gate releases; fireworks as client 3.
M7  Fleet viewer v2: panel-hosted WS forwarder (post-verification
    frames; one-client cap for RAM) + terrain twin in-browser from
    ported pure world functions, golden-hashed against M2.

## 6. Non-goals (v1)
Dynamic membership beyond rejoin; WAN operation; >16 nodes;
persistence of sim state (derived by design); BFT.
