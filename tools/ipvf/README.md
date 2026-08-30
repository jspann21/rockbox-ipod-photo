# IPVF native video and audio

IPVF is the native media format for Rockbox on the iPod Photo/Color. The host
does scaling, frame-rate conversion, RGB conversion, audio resampling, LZ4
compression, and IMA ADPCM encoding once. The device expands lossless 220x176
`rgb565be` display records and decodes their audio directly into Rockbox's
playback mixer ring.

## Encode

Install `ffmpeg`, then run:

```sh
python3 tools/ipvf/encode.py input.mp4 output.ipvf --fps 30
```

Use `--fps 60` for a 60 fps file. `encode.py` is the only IPVF encoder;
`test_encode.py` is a small host-only unit-test module for the format contract.

The source must contain an audio stream. The encoder converts the first audio
stream to 44.1 kHz stereo signed 16-bit PCM, then stores each frame's time slice
as stereo IMA ADPCM. The stored audio is about 44,100 bytes per second, or 2.65
MB per minute, plus an eight-byte state header per video frame. Predictors are
anchored to exact samples at every record boundary, while step indices continue
across records for better quality. Every block remains independently decodable.

The encoder preserves aspect ratio, letterboxes to 220x176, and chooses among
full keyframes, repeat records, and lossless rectangular patches. Raw video is
replaced by an independent LZ4 block only when the complete sector-aligned
record becomes smaller. Its standard-library-only compressor searches bounded
hash chains, prefers longer matches, uses one-byte lazy matching, and avoids
expensive short-offset copies when an almost-equivalent match is available.
Those choices reduce both stored bytes and PP5020 match-copy work. The encoder
forces a keyframe every 120 frames by default. `--keyint 0` disables forced
keyframes.

IPVF requires at least 4 fps. Every stored record is limited to 192 sectors (96
KiB), so it fits the player's dedicated read buffer even when incompressible
video takes the raw fallback. If decoded audio is longer than the converted
video, it is trimmed. If it is shorter, the encoder appends silence so video and
audio have exactly the same duration.

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
| 20 | 4 | flags: `11` (`RGB565BE | SECTOR_RECORDS | IMA_ADPCM`) |
| 24 | 4 | first record offset: `512` |
| 28 | 2 | first record size in 512-byte sectors |
| 30 | 2 | audio format: `2` (`IMA_ADPCM`) |
| 32 | 2 | channels: `2` |
| 34 | 2 | bits per sample: `16` |
| 36 | 4 | sample rate: `44100` |
| 40 | 4 | total decoded stereo sample frames |
| 44 | 468 | zero padding |

Each video frame is one whole-sector record:

| Record offset | Size | Meaning |
| ---: | ---: | --- |
| 0 | 1 | type: key raw `0`, rectangles raw `1`, repeat `2`, key LZ4 `3`, rectangles LZ4 `4` |
| 1 | 1 | rectangle count |
| 2 | 2 | next record size in sectors; zero only for the final frame |
| 4 | 4 | stored video bytes |
| 8 | 4 | decoded video bytes |
| 12 | variable | raw or independent LZ4 video block |
| after stored video | variable | matching stereo IMA ADPCM block |
| end | variable | zero padding to a 512-byte boundary |

A key video payload is one 77,440-byte frame. A rectangle payload contains one
or more `<x, y, width, height, byte_count>` headers followed by row-major
pixels. The encoder aligns `x` and `width` to complete two-pixel LCD words. A
compressed record expands to exactly the corresponding key or rectangle
payload; blocks do not depend on prior LZ4 history. A repeat has no video
payload, but still contains the audio block for its time span.

For frame `n`, define the cumulative audio boundary:

```text
S(n) = round(n * 44100 * fps_den / fps_num)
```

For `N = S(n + 1) - S(n)`, the record's audio block is exactly `8 + N - 1`
bytes. Its eight-byte header stores a signed 16-bit predictor, step index, and
zero reserved byte for the left channel, followed by the same fields for the
right channel. Those predictors are decoded sample frame zero. Each remaining
byte stores the next left code in its low nibble and right code in its high
nibble. The header's total audio count must be `S(frame_count)`, making record
sizes and A/V duration independently verifiable without per-record counts.

The player accepts only the layout above.

## Device implementation

On PP5020 hardware, the player preserves the qualified three-slot pipeline:

1. The CPU issues one sector-aligned read into a dedicated uncached 96 KiB
   staging buffer. Video and its matching IMA block arrive in that read.
2. The CPU decodes the record's IMA samples into a power-of-two mixer ring
   obtained from Rockbox's plugin audio buffer. Raw video is copied directly to
   an acquired render slot. LZ4 video is expanded into cached scratch and then
   copied once to that slot, avoiding byte-at-a-time writes to uncached SDRAM.
   LZ4 literals use Rockbox's bulk copy from the uncached read buffer; short
   cached match copies stay inline.
3. The COP sends the prior decoded slot through the target LCD driver while the
   CPU reads and decodes the next record.
4. After the first frame and its audio are decoded and the image is presented,
   the CPU scans future records to prebuffer about one second of audio, seeks
   back to the second record, and starts Rockbox's playback mixer at 44.1 kHz.
   Consumed PCM samples become the master clock for later video frames.
5. If storage ever outruns the audio ring, the mixer channel stops and restarts
   when the next record arrives. The audio clock pauses with it, preserving A/V
   alignment instead of silently drifting.
6. Displayed slots are released immediately. At exit, the CPU rereads and
   decodes from the last keyframe, reconstructs the final framebuffer, and
   commits it once so Rockbox resumes with coherent state.

The target driver retains the per-word LCD2 readiness handshake. The player
does not use raw plugin MMIO, cache invalidation, invented LCD DMA requests, or
a new PCM interrupt path.

The three decoded render slots are each 128 KiB apart and are followed by one
shared 96 KiB record buffer. Their base requires only 512-byte sector alignment.
Compressed input and decoded output never overlap.

## Host format validation

`test_encode.py` does not create a second IPVF format or device path. It calls
the production encoder with synthetic frames and PCM, then parses the result to
catch broken headers, record chains, audio placement, duration, and size limits.
Run it with:

```sh
python3 -m unittest -v tools.ipvf.test_encode
```

They verify the canonical header, raw/LZ4/repeat sector chaining, LZ4 roundtrip
and malformed-input rejection, IMA block sizing and predictors, silence
padding, trimming to the converted video duration, and the 96 KiB record bound.

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

The final LZ4-video/IMA-audio path passed A1099 qualification with an 8-second
music-video clip generated from the same source at both rates:

| Clip | File bytes | Frames | Late | Audio gaps | Result |
| --- | ---: | ---: | ---: | ---: | --- |
| LZ4HC + IMA, 30 fps | 4,640,768 | 240 | 0 | 0 | video and audio visually/audibly correct |
| LZ4HC + IMA, 60 fps | 4,713,984 | 480 | 41 | 0 | video and audio visually/audibly correct |

These files are about 51% smaller than the corresponding 9,492,992-byte and
9,590,272-byte PCM candidates. Both contain exactly 352,800 decoded stereo
sample frames, and all 240/480 sector records passed complete host decoding and
frame reconstruction before device testing. A late count means the consumed
audio clock was more than 500 microseconds past a frame boundary; it is not a
dropped-frame count. The 41 events at 60 fps produced no visible defect.

Broader qualification still includes a long drift run, line out, deliberate
storage stalls, Menu stop, USB insertion, and repeat-heavy audio content.
