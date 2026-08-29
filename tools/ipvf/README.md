# IPVF native video

IPVF is the native video format for Rockbox on the iPod Photo/Color. The host
does scaling, frame-rate conversion, and RGB conversion once; the device reads
lossless 220x176 `rgb565be` pixels in the exact byte order used by its
`RGB565SWAPPED` display.

## Encode

Install `ffmpeg`, then run:

```sh
python3 tools/ipvf/encode.py input.mp4 output.ipvf --fps 30
```

The encoder preserves aspect ratio, letterboxes to 220x176, and chooses among
full keyframes, repeat records, and lossless rectangular patches. It forces a
keyframe every 120 frames by default. `--keyint 0` disables forced keyframes.

## File layout

All integers are little-endian. The first record begins at byte 512.

| Offset | Size | Meaning |
| ---: | ---: | --- |
| 0 | 4 | `IPVF` |
| 4 | 2 | format version: `1` |
| 6 | 2 | logical header size: `64` |
| 8 | 2 | width: `220` |
| 10 | 2 | height: `176` |
| 12 | 2 | frame-rate numerator |
| 14 | 2 | frame-rate denominator |
| 16 | 4 | frame count |
| 20 | 4 | flags: `3` (`RGB565BE | SECTOR_RECORDS`) |
| 24 | 4 | first record offset: `512` |
| 28 | 2 | first record size in 512-byte sectors |
| 30 | 482 | zero padding |

Each frame is one whole-sector record:

| Record offset | Size | Meaning |
| ---: | ---: | --- |
| 0 | 1 | type: key `0`, rectangles `1`, repeat `2` |
| 1 | 1 | rectangle count |
| 2 | 2 | next record size in sectors; zero only for the final frame |
| 4 | 4 | payload size in bytes |
| 8 | variable | payload |
| end | variable | zero padding to a 512-byte boundary |

A key payload is one 77,440-byte frame. A rectangle payload contains one or
more `<x, y, width, height, byte_count>` headers followed by row-major pixels.
The encoder aligns `x` and `width` to complete two-pixel LCD words. A repeat has
no payload.

The player rejects the earlier unaligned prototype layout. IPVF is defined by
the layout above; there is no parallel legacy mode.

## Device implementation

On PP5020 hardware, the player uses three 128 KiB-aligned slots:

1. The CPU issues one sector-aligned read directly into an uncached slot.
2. The COP sends the previous slot through the target LCD driver while the CPU
   reads the next record.
3. Displayed slots are released immediately; the framebuffer is not copied for
   every frame.
4. At exit, the CPU rereads from the last keyframe, reconstructs the final
   framebuffer, and commits it once so Rockbox resumes with coherent state.

The target driver retains the per-word LCD2 readiness handshake. The player
does not use raw plugin MMIO, cache invalidation, invented LCD DMA requests, or
runtime marker files. Simulator builds use the same canonical parser with a
sequential framebuffer renderer.

## Hardware result

The canonical encoder/player pair passed these A1099 tests with exact final
framebuffer CRCs and no visual defects:

| Clip | Frames | Late | Final CRC |
| --- | ---: | ---: | --- |
| high-motion 30 fps | 240 | 0 | `5b2183e9` |
| high-motion 60 fps | 480 | 0 | `d9a00934` |
| local-motion 60 fps | 480 | 0 | `97f4a351` |

For the high-motion 60 fps clip, aligned record reads reduced measured read
time from 5.318 seconds to 1.932 seconds. The local-motion test included 193
repeat records and a 120-record final framebuffer reconstruction.
