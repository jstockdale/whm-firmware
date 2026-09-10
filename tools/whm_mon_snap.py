#!/usr/bin/env python3
"""Decode a whm-monitor serial 'snap' (base64 RGB565) to BMP.
Usage: python3 whm_mon_snap.py serial.log [out.bmp]
Feed it a log containing the SNAP BEGIN/END block."""
import sys, base64, struct
if len(sys.argv) < 2:
    print(__doc__); sys.exit(1)
txt = open(sys.argv[1], "r", errors="ignore").read()
try:
    body = txt.split("SNAP BEGIN")[1].split("SNAP END")[0]
except IndexError:
    print("no SNAP block found"); sys.exit(1)
hdrline, _, b64 = body.partition("\n")
W, H = 536, 240
raw = base64.b64decode("".join(b64.split()))
assert len(raw) == W * H * 2, f"size {len(raw)} != {W*H*2}"
out = sys.argv[2] if len(sys.argv) > 2 else "snap.bmp"
rowb = W * 3
with open(out, "wb") as f:
    f.write(b"BM" + struct.pack("<IHHI", 54 + rowb * H, 0, 0, 54))
    f.write(struct.pack("<IiiHHIIiiII", 40, W, -H, 1, 24, 0,
                        rowb * H, 0, 0, 0, 0))
    for y in range(H):
        row = bytearray()
        for x in range(W):
            c = raw[(y * W + x) * 2] | (raw[(y * W + x) * 2 + 1] << 8)
            row += bytes(((c << 3) & 0xF8, (c >> 3) & 0xFC,
                          (c >> 8) & 0xF8))
        f.write(row)
print(f"wrote {out} ({W}x{H})")
