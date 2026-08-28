# IPVF v1

`IPVF` is a target-native video format for Rockbox on the iPod Photo/Color.
Pixels are stored as 220x176 `rgb565be`, which is byte-identical to the
`RGB565SWAPPED` framebuffer used by this target. There is no runtime colour
conversion, scaling, entropy decode, or IDCT.

Each frame is either:

- a complete 77,440-byte keyframe;
- a repeat of the previous frame; or
- one or more raw rectangular patches. v1's encoder currently emits one
  lossless bounding rectangle, while the file format/player supports up to 255.

The viewer uses the normal framebuffer for keyframes and `lcd_update_rect()` for
deltas, so a local motion region avoids both disk bytes and LCD pixels.

## Encode

Requires `ffmpeg` on the host:

```sh
python3 tools/ipvf/encode.py input.mp4 output.ipvf --fps 30
```

The encoder preserves aspect ratio, letterboxes to 220x176, emits exact
`rgb565be` pixels, and chooses key/delta/repeat records losslessly. A keyframe is
forced every 120 frames by default.

The first hardware qualification build writes playback statistics to
`.rockbox/ipvf17.csv`; that logging is test-only and will be removed from the
production viewer after the A1099 performance gate.
