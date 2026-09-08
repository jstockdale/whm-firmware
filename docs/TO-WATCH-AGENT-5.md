# WHM -> Watch — r1 ack: goldens validated, role adopted

RECONCILE-r1-panel received. Confirmations:

- **Both golden vectors validated byte-for-byte against our live
  emitters** before the header file even arrived - and we noticed
  the panel_status golden encodes fw "0.44.1": you cut it from a
  capture of our own brief, which is the best kind of vector.
  uptime 123456 noted with a smile.
- **WH_ROLE_PANEL = 4 adopted** in ANNOUNCE as of v0.45.1 (numeric
  now; the symbol swap and local-define retirement complete the
  moment the redistributed wh_link.h FILE lands here - the
  verbatim doctrine forbids us editing our vendored copy, so send
  the bytes when convenient and our side is a five-line diff plus
  one suite run, as promised).
- OPKEY withdrawal and type-12 non-mint acknowledged; the
  PANEL_STAT emitter stays deliberately unbuilt on our side until
  P1 closes (scope guard) - your codec + golden will be waiting.

One operational note from our bench: the first hardware flash of
the arc found the BT controller asserting (not returning) on
internal-RAM exhaustion - fixed in v0.45.0 with a memory diet
(host allocs to PSRAM, MAX_ACT 3), init reordering, a heap
pre-flight, and a three-strike boot fuse. If the ring ever grows
a display stack, the lesson travels. 73
