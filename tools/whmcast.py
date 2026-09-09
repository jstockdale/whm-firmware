#!/usr/bin/env python3
"""whmcast.py - live-cast screen or file to the fleet (docs/MEDIA.md)."""
import argparse, time, requests
from PIL import Image

def rle(im, w, h):
    px = im.convert("RGB").load(); out = bytearray()
    for y in range(h):
        x = 0
        while x < w:
            r, g, b = px[x, y]; run = 1
            while x+run < w and run < 255 and px[x+run, y] == (r, g, b):
                run += 1
            out += bytes((run, r, g, b)); x += run
    return bytes(out)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="one.local")
    ap.add_argument("--size", default="128x64")
    ap.add_argument("--fps", type=float, default=10)
    ap.add_argument("--file")
    a = ap.parse_args()
    w, h = map(int, a.size.lower().split("x"))
    url = f"http://{a.host}/stream/frame"; t0 = time.time()
    if a.file:
        from whmconv import frames_of
        for i, fr in enumerate(frames_of(a.file, w, h, a.fps)):
            requests.post(url, params={"w": w, "h": h,
                          "pts": int(i*1000/a.fps)},
                          data=rle(fr, w, h), timeout=2)
            time.sleep(max(0, (t0+(i+1)/a.fps)-time.time()))
    else:
        import mss
        with mss.mss() as s:
            mon = s.monitors[1]
            while True:
                shot = s.grab(mon)
                im = Image.frombytes("RGB", shot.size, shot.bgra,
                                     "raw", "BGRX").resize((w, h))
                pts = int((time.time()-t0)*1000)
                try:
                    requests.post(url, params={"w": w, "h": h,
                                  "pts": pts},
                                  data=rle(im, w, h), timeout=2)
                except Exception as e: print("post:", e)
                time.sleep(1/a.fps)

if __name__ == "__main__": main()
