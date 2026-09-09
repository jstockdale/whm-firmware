# P1 - the .whm media pipeline and the LIVE lane

## Container (`.whm`)
32-byte header, little-endian:

    "WHMV" | ver u8 | flags u8 | w u16 | h u16 | fps_q8 u16
    nframes u32 | loop u32 | reserved[14]

Then per frame: `size u32` + payload. Payload = **RGB888 row-RLE**:
each row is runs of `[len u8 (1..255), r, g, b]` until `w` pixels are
emitted. Frames are independent - every frame is a seek point.

Fleet-wide files (`w > 64`) are cropped per node at `x = strip*64`:
one 128x64 movie spans two panels because the format never knew the
bezel existed.

## Playback clock
`frame = (tsf - start_tsf) * fps / 1e6` - pure f(TSF), the same
physics that locked the walker and the music. `fleet media play f.whm`
boundary-starts every node on the same 500ms TSF grid slot.

## LIVE lane
Any encoder POSTs frames (same RLE) to the anchor:

    POST http://<anchor>/stream/frame?w=128&h=64&pts=<ms>   body=RLE

The anchor displays its crop and fans **type-15 FRAME** fragments
(<=1100B) to the fleet. Every node maps `pts -> tsf` off the first
frame (+150ms latency budget) and schedules identically: live video
hits both panels at the same TSF instant. Loss drops a frame and
holds the last.

Two clients ship:
- **Browser** (zero install): open `http://<anchor>/live`, pick
  screen-share / webcam / a video file - the page downscales,
  RLE-encodes in JS, and streams.
- **CLI**: `tools/whmcast.py --host one.local` (screen via mss, or
  `--file movie.mp4` via ffmpeg).

## Converter
`tools/whmconv.py in.gif out.whm --size 128x64 --fps 12 --loop`
(Pillow; mp4/etc. via ffmpeg). Copy `.whm` files to `/sdcard/media/`
or `sync media <host>` replicates them.
