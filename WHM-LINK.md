# WHM-LINK - fleet protocol design

Status: P2a implemented (roles, announce, fleet wall clock). Later layers
specified here, sequenced at the bottom. This document is the deep dive;
ROADMAP.md holds the schedule.

## 0. Radio & association

One node is the **conductor**: it runs the SoftAP we already ship
(SSID = the node name, WPA2, default PSK - per-node secret is a flagged P4
item). **Members** are plain STAs joined to it. Off-grid is first-class:
a generator, three panels, zero infrastructure.

Rejected alternatives, with reasons:
- **ESP-NOW**: no TSF. We would hand-timestamp at the application layer
  and inherit scheduling jitter; 802.11 beacons are timestamped by the
  MAC hardware for free.
- **ESP-MESH**: multi-hop we don't need at this scale, at the price of
  latency layers between us and the TSF.
- Future hybrid: the conductor may run APSTA - member-facing AP plus a
  STA leg into house WiFi - bridging real SNTP into the flock. The
  protocol below doesn't change.

Capacity: SoftAP default 4 stations (configurable to ~10). A conductor
plus nine members per channel is the ceiling before we need a channel
plan; irrelevant until the fleet outgrows a dining table.

## 1. Time - the whole point

**Show clock = the conductor's TSF.** Every member's WiFi MAC
hardware-latches the conductor's beacon timestamp; `esp_wifi_get_tsf_time`
then reads a microsecond clock shared across the flock to well under
±10µs, drift-corrected every 102.4ms beacon. Bench-measured stability on
unit #1: ±1ppm between beacons. All scheduling below is expressed as
"do X at TSF=T".

**Wall clock rides the show clock.** The conductor owns one anchor pair
`(epoch_us, tsf_us)` sampled at the same instant, broadcast in its
announce packet. A member computes
`epoch_now = anchor.epoch + (tsf_now - anchor.tsf)` - microsecond-class
wall time with no NTP, no round-trip estimation, because the transport
IS the clock. Members apply it only when they have no SNTP sync of
their own, write it through to their RTC, and thereafter every clock
face, blinking colon, and seconds-bar flash in the flock moves as one.

## 2. Control & state - "whmcast"

UDP broadcast, port 7777, little-endian packet:

    magic 'WHML' | ver u8 | type u8 | seq u16 | name char[16] | payload

Types:
- **ANNOUNCE** (conductor 1Hz, members 0.2Hz): role, fw version,
  capability bits, the epoch/TSF anchor, media manifest hash, and a
  small sensor snapshot (T, RH, VU level). Doubles as presence,
  time source, and a free fleet sensor mesh.
- **CMD** (any→all, P2c+): a console line to execute -
  `screen life`, `brightness 40`, `text HELLO`, `play <sha> at <tsf>`.
  Fleet control = the console we already have, fanned out.
- **EVENT** (P4): button gestures, timer expiry - any node's input can
  drive the flock.

Security posture: the WPA2 link already gates membership and encrypts
frames; an application-layer HMAC (PSK-derived) is specified for P4
alongside the per-node-secret work, not before.

## 3. File distribution - the "p2p" layer

Any node can serve; any node can pull. Plain HTTP on port 7778:

    GET /manifest            -> lines of: name size sha256   (of /sdcard/media)
    GET /media/<name>        -> bytes (Range supported)

`sync media <node>` diffs the local manifest against the peer's and
pulls what's missing or mismatched, straight to SD. At our measured
0.94MB/s SD write floor, a 5MB track lands in ~6s; a full evening's set
in minutes, once, then it's local.

**Pushback recorded:** a chunked swarm protocol (BitTorrent-style) buys
nothing at fleet size ≤10 with one radio hop - the bottleneck is SD
write, not the link, and any node can already be the seed. The "p2p"
property that matters - no external server, no privileged node - is
fully present. The same HTTP server is the P1 web UI's and P4 OTA's
foundation, so this layer is paid for three times over.

## 4. Synchronized audio - two modes, one engine

Human echo perception: <1ms reads as one source, ~5ms as ambience,
>10ms as comb/echo. Boards' crystals disagree by ±20ppm → 1ms skew per
50s of playback. Conclusion: **a one-shot synchronized start is not
enough**; every player must continuously discipline its sample position
to the show clock.

**The engine (shared kernel):** playback maintains
`expected_frames = (tsf_now - start_tsf) * fs / 1e6` and compares with
frames actually written. Every 250ms, if |error| > 500µs, insert or
drop the few frames needed to re-center (with hysteresis). Occasional
single-frame surgery at 44.1kHz is inaudible; the bound it buys -
±0.5ms flock-wide, forever - beats our video flip quantization.

**Mode A - replicate + schedule (build first):** distribute the file
(layer 3), then conductor broadcasts `PLAY {sha, start_tsf}` with
start_tsf ~1s out; every node decodes locally through the engine.
Robust to any WiFi hiccup mid-song (the file is local), zero streaming
protocol, and it exercises the engine end to end.

**Mode B - live stream (the DJ mode):** the source node sends its
*compressed* MP3 frames, each stamped with a presentation TSF, over
per-member TCP (3 members × 320kbps = trivial; TCP's retransmit beats
hand-rolled FEC at this scale - pushback recorded against UDP
multicast, whose frames go out at low basic rate, unacknowledged).
Members buffer ~1s, decode, and feed the same engine using embedded
PTS. Lost-connection concealment: repeat last frame (26ms), rejoin at
next PTS. Latency vs the DJ's button press = the buffer; fine for
ambient, and the buffer is a knob. Mode B reuses ~90% of Mode A and
also opens future line-in/mic streaming.

## 5. Other data the fabric supports (ideas register)

- Fleet command fan-out (`fleet <any console line>`).
- Sensor mesh: ANNOUNCE already carries T/RH/VU - render a house map
  on any panel, or aggregate on the conductor for the web UI.
- Spatial VU: three mics, three panels, one room-scale meter.
- **Life without borders**: cells crossing a panel's edge emigrate via
  UDP to the neighbor's opposite edge - one colony across the flock.
- The wanderer (P2 seam test) generalizes to any multi-panel sprite;
  pixel-pet visits other panels.
- Oracle asked on one panel, answer materializes on another.
- Any button pauses all music (EVENT type).

## 6. Sequencing

- **P2a (shipped, 0.8.0):** roles + NVS persistence, announce/recv
  tasks, fleet wall clock via anchor, `sync` console command, peer
  table. Two units can link today.
- **P2b:** wanderer across two panels - the visual seam acceptance test
  (flip-quantization verdict lives or dies here).
- **P2c:** HTTP file service + `sync media` + CMD fan-out.
- **P2d:** Mode A audio (engine + scheduled start).
- **P2e:** Mode B live streaming on the same engine.

## 7. Linking two units (today's procedure)

    unit1> name stage-left        unit2> name stage-right
    unit1> sync conduct           unit2> sync join stage-left
    both>  sync status            (roles, peers, anchor age, TSF)

Member wall clocks, colons, and seconds-bar flashes lock to the
conductor within one announce (≤1s). Roles persist across reboots; the
flock self-assembles on power-up.
