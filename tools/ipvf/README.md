# IPVF native video and audio

IPVF is the native media format for Rockbox on the iPod Photo/Color. The host
does scaling, frame-rate conversion, RGB conversion, and audio resampling once;
the device reads lossless 220x176 `rgb565be` pixels in the exact byte order
used by its `RGB565SWAPPED` display and sends native PCM to Rockbox's playback
mixer.

## Encode

Install `ffmpeg`, then run:

```sh
python3 tools/ipvf/encode.py input.mp4 output.ipvf --fps 30
```

Use `--fps 60` for a 60 fps file. `encode.py` is the only IPVF encoder;
`test_encode.py` is a small host-only unit-test module for the format contract.

The source must contain an audio stream. The encoder converts the first audio
stream to 44.1 kHz stereo signed 16-bit little-endian PCM. PCM contributes
176,400 bytes per second, or about 10.6 MB per minute. The encoder preserves
aspect ratio, letterboxes to 220x176, and chooses among full keyframes, repeat
records, and lossless rectangular patches. It forces a keyframe every 120
frames by default. `--keyint 0` disables forced keyframes.

IPVF requires at least 4 fps so a worst-case keyframe plus its matching PCM
slice fits the player's 128 KiB record slot. If decoded audio is longer than the
converted video, it is trimmed. If it is shorter, the encoder appends silence
so video and audio have exactly the same duration.

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
| 20 | 4 | flags: `7` (`RGB565BE | SECTOR_RECORDS | PCM_S16LE`) |
| 24 | 4 | first record offset: `512` |
| 28 | 2 | first record size in 512-byte sectors |
| 30 | 2 | audio format: `1` (`PCM_S16LE`) |
| 32 | 2 | channels: `2` |
| 34 | 2 | bits per sample: `16` |
| 36 | 4 | sample rate: `44100` |
| 40 | 4 | total stereo PCM sample frames |
| 44 | 468 | zero padding |

Each video frame is one whole-sector record:

| Record offset | Size | Meaning |
| ---: | ---: | --- |
| 0 | 1 | type: key `0`, rectangles `1`, repeat `2` |
| 1 | 1 | rectangle count |
| 2 | 2 | next record size in sectors; zero only for the final frame |
| 4 | 4 | video payload size in bytes |
| 8 | variable | video payload |
| after video | variable | matching PCM slice |
| end | variable | zero padding to a 512-byte boundary |

A key video payload is one 77,440-byte frame. A rectangle payload contains one
or more `<x, y, width, height, byte_count>` headers followed by row-major
pixels. The encoder aligns `x` and `width` to complete two-pixel LCD words. A
repeat has no video payload, but still contains the PCM slice for its time span.

For frame `n`, define the cumulative audio boundary:

```text
S(n) = round(n * 44100 * fps_den / fps_num)
```

The record contains exactly `S(n + 1) - S(n)` interleaved stereo PCM sample
frames immediately after its video payload. The header's total audio count must
be `S(frame_count)`. This makes record sizes and A/V duration independently
verifiable without per-record audio metadata.

The player accepts only the layout above.

## Device implementation

On PP5020 hardware, the player preserves the qualified three-slot pipeline:

1. The CPU issues one sector-aligned read directly into an uncached 128 KiB
   slot. Video and the matching PCM slice arrive in that same read.
2. The CPU copies the PCM slice into a power-of-two audio ring obtained from
   Rockbox's plugin audio buffer.
3. The COP sends the slot's RGB payload through the target LCD driver while the
   CPU reads the next record.
4. After the first frame is fully presented, the plugin starts Rockbox's
   playback mixer at 44.1 kHz. Consumed PCM samples become the master clock for
   later video frames.
5. If storage ever outruns the audio ring, the mixer channel stops and restarts
   when the next record arrives. The audio clock pauses with it, preserving A/V
   alignment instead of silently drifting.
6. Displayed slots are released immediately. At exit, the CPU rereads from the
   last keyframe, reconstructs the final framebuffer, and commits it once so
   Rockbox resumes with coherent state.

The target driver retains the per-word LCD2 readiness handshake. The player
does not use raw plugin MMIO, cache invalidation, invented LCD DMA requests, or
a new PCM interrupt path.

The three render slots are each 128 KiB apart, but their shared base requires
only 512-byte sector alignment. Treating the slot capacity as a base-alignment
requirement wastes memory and can reject a valid plugin buffer.

## Host format validation

`test_encode.py` does not create a second IPVF format or device path. It calls
the production encoder with synthetic frames and PCM, then parses the result to
catch broken headers, record chains, audio placement, duration, and size limits.
Run it with:

```sh
python3 -m unittest -v tools.ipvf.test_encode
```

They verify the canonical header, key/delta/repeat sector chaining, exact PCM
placement, silence padding, trimming to the converted video duration, and the
128 KiB keyframe limit at 4 fps.

## Hardware qualification status

The framebuffer-native video and aligned CPU/COP display pipeline have already
been hardware-qualified on an A1099:

| Clip | Frames | Late | Final CRC |
| --- | ---: | ---: | --- |
| high-motion 30 fps | 240 | 0 | `5b2183e9` |
| high-motion 60 fps | 480 | 0 | `d9a00934` |
| local-motion 60 fps | 480 | 0 | `97f4a351` |

For the high-motion 60 fps clip, aligned record reads reduced measured read
time from 5.318 seconds to 1.932 seconds. The local-motion test included 193
repeat records and a 120-record final framebuffer reconstruction.

The integrated PCM path has also passed its first A1099 qualification with an
8-second music-video clip:

| Clip | Frames | Late | Audio gaps |
| --- | ---: | ---: | ---: |
| PCM video, 30 fps | 240 | 0 | 0 |
| PCM video, 60 fps | 480 | 0 | 0 |

Both files contained exactly 352,800 stereo PCM sample frames, and all 240/480
sector records passed the host validator before device testing. Broader
qualification still includes a long drift run, line out, deliberate storage
stalls, Menu stop, USB insertion, and repeat-heavy audio content.
