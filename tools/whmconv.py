#!/usr/bin/env python3
"""whmconv.py - anything -> .whm (docs/MEDIA.md)."""
import sys, struct, argparse, subprocess, tempfile, os, glob
from PIL import Image, ImageSequence

def rle_frame(im, w, h):
    px = im.convert("RGB").load(); out = bytearray()
    for y in range(h):
        x = 0
        while x < w:
            r, g, b = px[x, y]; run = 1
            while x + run < w and run < 255 and px[x+run, y] == (r, g, b):
                run += 1
            out += bytes((run, r, g, b)); x += run
    return bytes(out)

def frames_of(path, w, h, fps):
    ext = os.path.splitext(path)[1].lower()
    if ext in (".gif", ".png", ".jpg", ".jpeg", ".webp", ".bmp"):
        im = Image.open(path)
        for fr in ImageSequence.Iterator(im):
            yield fr.copy().convert("RGB").resize((w, h), Image.LANCZOS)
    else:
        with tempfile.TemporaryDirectory() as td:
            subprocess.run(["ffmpeg", "-v", "error", "-i", path,
                            "-vf", f"fps={fps},scale={w}:{h}",
                            f"{td}/f%06d.png"], check=True)
            for f in sorted(glob.glob(f"{td}/f*.png")):
                yield Image.open(f)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("inp"); ap.add_argument("out")
    ap.add_argument("--size", default="128x64")
    ap.add_argument("--fps", type=float, default=12)
    ap.add_argument("--loop", action="store_true")
    a = ap.parse_args()
    w, h = map(int, a.size.lower().split("x"))
    pls = [rle_frame(f, w, h) for f in frames_of(a.inp, w, h, a.fps)]
    hdr = struct.pack("<4sBBHHHII12x", b"WHMV", 1, 0, w, h,
                      int(a.fps * 256), len(pls), 1 if a.loop else 0)
    with open(a.out, "wb") as f:
        f.write(hdr)
        for p in pls:
            f.write(struct.pack("<I", len(p))); f.write(p)
    print(a.out, len(pls), "frames", f"{os.path.getsize(a.out)/1024:.1f}KB")

if __name__ == "__main__": main()
