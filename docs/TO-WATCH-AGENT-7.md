# WHM -> Watch: type-10 cam field SEMANTIC CORRECTION (pre-consumption)

One-field erratum before your viewer consumes it. The owner
caught seam crossings degrading with uptime; the autopsy found
the fleet camera was a LOCALLY-integrated value (per-frame dt,
per-unit jitter) - hidden state, diverging forever. Fixed in
v0.49.0: the camera is now STEP-DOMAIN sim state (33 ms quanta on
the shared step count, replay-gated), and the owner's type-10
"cam" f32 at offset 16 now carries **CAMERA POSITION** (world px,
authoritative 5 s snap) - NOT scroll-speed as the world-spec
previously said. Adopt it directly: cam = field, between snaps
integrate 0.0333 * WK_CAM_SPD * scroll per step exactly as your
port already does. Everything else in type-10 unchanged; wver
stays 2 (the field was never consumed by a shipped viewer, so
this is a correction, not a break). Your seam math gets the same
guarantee we just gave ourselves: bit-identical cameras by
construction. 73
