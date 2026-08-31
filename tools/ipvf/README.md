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
`test_encode.py` and `test_lab.py` are small host-only contract suites.

The source must contain an audio stream. The encoder converts the first audio
stream to 44.1 kHz stereo signed 16-bit PCM, then stores each frame's time slice
as stereo IMA ADPCM. The stored audio is about 44,100 bytes per second, or 2.65
MB per minute, plus an eight-byte state header per video frame. Predictors are
anchored to exact samples at every record boundary, while step indices continue
across records for better quality. Every block remains independently decodable.

The encoder preserves aspect ratio, letterboxes to 220x176, and adaptively
chooses among full keyframes, repeats, lossless rectangular patches, bounded
multi-rectangle patches, and temporal XOR+LZ4. A more expensive representation
is selected only when the complete record, including audio and 512-byte
rounding, becomes at least one sector smaller. Temporal records carry the
Rockbox CRC32 of their compressed residual payload. The built-in compressor
searches bounded hash chains, prefers longer matches, uses one-byte lazy
matching, and avoids expensive short-offset copies when an almost-equivalent
match is available. By default, the host also tries official LZ4HC level 12
when `liblz4` is available and keeps the smaller raw LZ4 block per record. This
changes no device format and cannot enlarge a record; use `--lz4-mode builtin`
for the original compressor alone. The encoder forces a true key every 120 frames by default;
temporal `auto` mode rejects `--keyint 0` so dependency chains stay bounded.
It also rejects rates above 30 fps because hardware lower-bound testing proved
that dense full-frame temporal reconstruction cannot meet the 60 fps budget.

The default `--video-mode spatial` evaluates the hardware-proven current
representation plus bounded multi-rectangle patches and selects a spatial
candidate only when it removes at least one complete sector. Use
`--video-mode current` for the original single-bounding-rectangle path.
Explicit `--video-mode auto` also adds experimental temporal XOR+LZ4.

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
| 20 | 4 | flags: base `11`; temporal files use `27` (`base | TEMPORAL_XOR`) |
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
| 0 | 1 | type: key raw `0`, rectangles raw `1`, repeat `2`, key LZ4 `3`, rectangles LZ4 `4`, temporal XOR+LZ4 `5` |
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

A type-5 video payload begins with a four-byte little-endian Rockbox CRC32 of
the compressed residual bytes, followed by an independent raw LZ4 block that
expands to exactly 77,440 bytes of `previous_frame XOR current_frame`. The
player verifies the smaller stored payload before LZ4 decoding, reconstructs
into a cached persistent reference, then copies the frame once into the
uncached render slot. Only true type-0/type-3 keys reset the dependency chain
and act as reconciliation or future seek anchors.

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
   cached match copies stay inline. Temporal residuals are XORed into a cached
   77,440-byte reference, CRC-checked, and bulk-copied once to the slot.
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
Run them with:

```sh
python3 -m unittest -v tools.ipvf.test_encode tools.ipvf.test_lab
```

They verify the canonical header, raw/LZ4/repeat sector chaining, LZ4 roundtrip
and malformed-input rejection, IMA block sizing and predictors, silence
padding, trimming to the converted video duration, and the 96 KiB record bound.

`validate.py` is the strict streaming file validator. It walks sector links,
checks all bounds and zero padding, decodes every LZ4 and IMA block,
reconstructs every video mode, verifies temporal payload CRCs, and can compare every
frame byte-for-byte with fresh FFmpeg output:

```sh
python3 tools/ipvf/validate.py --source input.mp4 output.ipvf
```

## Deterministic corpus and compression lab

Generate the standard seeded lossless corpus in WSL, then run the complete
sector-cost matrix:

```sh
python3 tools/ipvf/generate_corpus.py \
  --out /tmp/ipvf-corpus --profile standard --format nut
python3 tools/ipvf/lab.py /tmp/ipvf-corpus/manifest.json \
  --output /tmp/ipvf-lab --jobs 8
```

The NUT/FFV1 sources are byte-identical across reruns with the same seed and
toolchain. The manifest records source/content hashes, exact rates, durations,
audio properties, motion class, and generator parameters. The lab compares
current/spatial/XOR paths, official LZ4 fast and HC levels, reversible RGB565
predictors, and 8x8/16x8/16x16 tile estimates using actual IMA sizes and
sector-rounded records. It writes JSONL, size/timing/summary CSVs, a Pareto
frontier, and run provenance.

The first 18-clip/718-frame standard run measured 6,279,168 aggregate record
bytes for the original current path, 6,115,840 for the existing spatial path,
and 5,927,936 for adaptive built-in/LZ4HC-12 compression. The latter is a
5.59% host-only saving with no decoder change. A Sub16+LZ4HC candidate reached
5,669,376 bytes (9.71% saving) but remains lab-only pending device inverse-loop
timing.

`profile_lab.py` benchmarks MP4-style host preprocessing on real footage. It
builds a beginning/middle/end sample, creates lossless profile sources, encodes
and strictly validates each IPVF, and reports complete size, SSIM, MB/minute,
and two-hour projections:

```sh
python3 tools/ipvf/profile_lab.py input.mp4 \
  --output /tmp/ipvf-profile-lab
```

On the 15-second `suds` real-footage sample, native 20 fps saved 15.5% with
SSIM 0.9838. RGB454 and RGB444 preprocessing at 24 fps saved 16.3% and 24.4%
with SSIM 0.9647 and 0.9562. Combining 20 fps with RGB454/RGB444 saved 29.1%
and 35.7%. Denoising alone saved only 1.3--2.4%. Lowering active resolution
saved bytes but damaged the measured image much more sharply, so it remains an
explicit compact-profile experiment rather than an everyday default. These
lossy candidates require an actual iPod LCD A/B before creator-profile
promotion; SSIM cannot judge motion judder or visible banding by itself.

The first A1099 LCD pass compared native-24, native-20, RGB454/24, RGB444/24,
and RGB444/20 versions of the same three scenes. Every clip was smooth with no
audio issue or immediate visible defect. Subjective motion preference still
favors 30 or possibly 60 fps over 20, so RGB444/20 is compact-only;
RGB444/24 is the leading tested cadence-preserving profile at 24.4% below
native-24. This 23.976-fps source cannot evaluate true 30/60 motion by frame
duplication; native-rate footage and host interpolation need separate tests.
This run also confirmed that the current player has no live volume control:
its button loop handles MENU stop and USB only.

## Hardware qualification status

The complete dated corpus, decoder-revision, telemetry, and acceptance record
is archived in
[`qualification/2026-08-31-hardware-runs.md`](qualification/2026-08-31-hardware-runs.md).

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

The first CRC-bearing adaptive temporal candidates passed host correctness and
paired A1099 format/lifecycle testing:

| Corpus/rate | Current bytes | Adaptive bytes | Saving |
| --- | ---: | ---: | ---: |
| high motion, 30 fps | 4,815,360 | 3,940,864 | 18.2% |
| high motion, 60 fps | 9,285,632 | 6,637,568 | 28.5% |
| music, 30 fps | 4,640,768 | 4,263,424 | 8.1% |
| music, 60 fps | 4,713,984 | 4,343,808 | 7.9% |
| local motion, 30 fps | 897,024 | 872,448 | 2.7% |
| local motion, 60 fps | 1,024,512 | 964,608 | 5.8% |

All reconstructed frames matched the earlier qualified corpus, and all 12
device runs completed without decoder or render errors. The initial temporal
decoder was not real-time: temporal peaks were 34--46 ms, causing audio gaps
when XOR records were frequent and at 60 fps. Temporal `auto` therefore remains
experimental.

A fused XOR/CRC experiment replaced the separate Rockbox space-optimized CRC
pass during qualification. Its telemetry separated LZ4, reconstruction, and
copy timing. The paired spatial-only pass saved
9.1% and 11.4% on the high-motion 30/60 fps clips without temporal records,
while correctly declining to expand the music files. Device playback improved
decode and render timing, had zero gaps at high-motion 30 fps, and had zero
gaps on both local/music 30/60 fps cases, so `spatial` is now the default.

A subsequent slicing-by-four CRC experiment was rejected on hardware: temporal
reconstruction rose from about 13.8 to 15.7 ms per XOR record, and music 60 fps
still visibly stuttered with 44 gaps. A bounded no-CRC diagnostic then measured
the lower bound before the checked payload-CRC implementation replaced it.

The no-CRC lower-bound pass measured plain aligned XOR at about 6.14 ms per
dependent frame. High-motion temporal 60 fps still averaged about 18 ms total
decode and logged 95 gaps; music temporal 60 fps visibly stuttered with 27.
Both 30 fps workloads reached zero gaps. The canonical type-5 integrity field
therefore now checks the much smaller compressed residual, and encoder `auto`
is capped at 30 fps. Spatial remains the default at every frame rate.

The checked payload-CRC implementation passed high-motion, music, and local
motion 30 fps playback with exact output and zero audio gaps. Payload checking
cost 0.34--5.96 ms per temporal record and aligned XOR about 6.22 ms. Temporal
`auto` is now the checked short-form 30 fps option; it remains non-default until
long-duration drift and lifecycle testing is complete.

Broader qualification still includes a long drift run, line out, deliberate
storage stalls, Menu stop, USB insertion, and repeat-heavy audio content.
Qualification telemetry is compile-time opt-in. Normal builds contain no TSV
logger/marker handling and perform no qualification timing or final-frame CRC
scan; define `IPVF_ENABLE_QUALIFICATION_TELEMETRY=1` only for an instrumented
qualification build. The production plugin is 13,924 bytes, down from 17,080
bytes for the final telemetry-enabled v5 qualification plugin.
