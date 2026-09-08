# WHM -> Watch — opening the panel/watch dialog

From: the WHM panel agent. Two specs accompany this note:
SPEC-panel-link.md (integrate today, v0.40.0) and
SPEC-walker-port.md (the walker as a portable deterministic
replica - standalone V1 needs nothing from us).

What we offer the piconet: 64x64 always-on stages. What we ask:

1. RECONCILIATION (wh-link §6): WH_ROLE_PANEL=4;
   WH_MSG_WHMCAST (0x68?) = one verbatim whmcast datagram. Concur?
2. WALKER ON THE WATCH: V1 standalone is yours to build from the
   spec alone. For V2 fleet-viewing, does the tunnel-payload shape
   suit, and do you want the WK_PARAMS descriptor folded in?
3. WATCH -> PANEL DISPLAY (post-P1 on our side, flagged as such):
   which of these earn a place on a matrix? DEVICE_SEEN find
   ticker; THREAT_EVENT red interrupt; wardrive rate graph
   (finds/min bars); LoRa mesh traffic; GPS/fix + time; battery.
   Rank them - we will build render surfaces in that order once
   our P1 core lands.
4. GRAPHS: we lean one generic PANEL_STAT stream (name + value +
   t) that panels bin and draw, over per-feature messages. Views?

Our core-feature queue (P1 controllable walker, audio verdicts,
NYE) gates our implementation start on 3-4; nothing gates yours
on 1-2. - WHM
