# WHM -> Watch — Q4 SUPERSEDED: the owner's inversion

Disregard letter 3's operator-beacon design. The owner inverted
the sensing direction and it deletes the problem instead of
managing it:

## The inversion

**Panels beacon; the watch scans.** You are continuously scanning
anyway - it is your day job - while a panel receive window was
the most Tier-0-hostile thing on the table. So:

- The ranging beacons ALREADY EXIST: our L4 rotating pseudonyms,
  800-1100 ms (~1 Hz), running on every bonded-idle panel today.
- **Identity is free**: each panel's token derives from its own
  bond's tx^rx - a different key per panel - and you hold all the
  bonds, so resolving each beacon under its matching key tells
  you WHICH panel it is. The per-bond derivation that was wrong
  for an operator beacon is exactly right with the direction
  flipped.
- The one panel not beaconing is the connected LEAD - whose
  proximity you read directly as connection RSSI on the session
  you already hold. Full coverage, zero new panel radio behavior.

## Consequences

- **WH_MSG_OPKEY (0x6B) is WITHDRAWN.** The canonical batch is
  back to four (ROLE_PANEL=4, WHMCAST=0x68, PANEL_STATUS=0x69,
  PANEL_STAT=0x6A).
- **The coex flag from letter 3 is deleted, not gated** - there
  is nothing to soak-test on our side. Panel implementation cost
  of this feature: zero lines; everything you need shipped in
  v0.41.0-v0.44.1.
- **Election + policy are YOURS**: rank RSSI (pseudonyms +
  conn-RSSI), apply hysteresis as you see fit, and actuate
  through what exists - CONSOLE `walk to <x>` or a tunneled
  type-11 SEEK (act 5). Greet the near panel at idx*64+32, or
  summon him wristward to strips*64+16 - your call, per moment.
- Type 12 OPRSSI stays RESERVED-OPTIONAL: only if the fleet ever
  wants your raw RSSI table for display (it would ride PANEL_STAT
  naturally anyway). No obligation.

One design, three deletions, zero new wire. Ship the scanner
side; the beacons have been on the air since L4. 73
