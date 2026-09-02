# IPVF native video and audio

IPVF is the native media format for Rockbox on the iPod Photo/Color. The host
does scaling, frame-rate conversion, RGB conversion, audio resampling, LZ4
compression, and IMA ADPCM encoding once. The device expands lossless 220x176
`rgb565be` display records and decodes their audio directly into Rockbox's
playback mixer ring.

## Encode

Install `ffmpeg`, then run:

```sh
python3 tools/ipvf/encode.py input.mp4 output.ipvf
```

That command now finishes by running the independent streaming inspector
against both the completed IPVF and fresh FFmpeg output from the source. A
successful run writes `output.ipvf.validation.json` with a compact pass record,
decoded-audio identity, mode counts, sizes, index count, and record bounds. An
encode that cannot pass independent reconstruction does not report success.
Use `--validation-report` only when the report needs a different location.

The default `everyday` profile preserves the source's exact rational cadence up
to 30 fps (including 24000/1001 and 30000/1001) and retains full RGB565
precision. The
complete 224.71-second file is 153,456,640 bytes. Full-device comparison found
noticeable gradient banding in RGB454 and very noticeable banding in RGB444,
while RGB555 remained noticeable for only an 8.40% saving. None is promoted
for normal use.

The `compact` profile keeps full RGB565 color and uses 20 fps. It produced
129,637,888 bytes, 15.52% below native, and passed exact source reconstruction.
No objective 20 fps playback or motion defect was observed; the name describes
its storage goal, not an established quality failure.

Use `--profile native` to retain full RGB565 precision, or `--profile compact`
for the 20 fps/RGB565 size-first option. `--fps` and `--color-depth` override
either profile for experiments. For a true 60 fps source, use `--profile
native --fps 60`; do not treat duplicated 24 fps frames as a 60 fps motion
test. `encode.py` is the only IPVF encoder; `test_encode.py` and `test_lab.py`
are small host-only contract suites.

The source must contain an audio stream. The encoder converts the first audio
stream to 44.1 kHz stereo signed 16-bit PCM, then losslessly chooses each
record's storage mode: zero bytes for exact digital silence, one-channel IMA for
exact dual mono, or ordinary stereo IMA. No threshold or automatic downmix is
used. Stereo costs about 44,100 bytes per second (2.65 MB/minute); dual mono is
about half. Predictors are anchored at every stored block boundary, while step
indices continue across records for quality. Every stored block remains
independently decodable.

IPVF also carries bounded UTF-8 `title`, `artist`, and `album` metadata.
The one-step creator imports those tags from the source when present; use
`--title`, `--artist`, or `--album` to override them. Metadata is descriptive
only and never changes decoding or compression.

Each encode also stores a content-derived media identity: the Rockbox CRC32 of
all complete sector-aligned media records. It remains stable when the file is
renamed and changes when encoded video or audio changes. The strict validator
recomputes it while walking records; the player uses it to key resume state.

The encoder preserves aspect ratio, letterboxes to 220x176, and adaptively
chooses among full keyframes, repeats, lossless rectangular patches, bounded
multi-rectangle patches, whole-frame translation residuals, and temporal
XOR+LZ4. A more expensive representation
is selected only when the complete record, including audio and 512-byte
rounding, becomes at least one sector smaller. Temporal records carry the
Rockbox CRC32 of their compressed residual payload. The built-in compressor
searches bounded hash chains, prefers longer matches, uses one-byte lazy
matching, and avoids expensive short-offset copies when an almost-equivalent
match is available. The `balanced` default uses official LZ4HC level 12 when
`liblz4` is available and otherwise falls back to the built-in compressor. This
changes no device format; use `--lz4-mode best` for an exhaustive size-first
comparison of both encoders, or `--lz4-mode builtin` for the original encoder
alone. The creator forces indexed true keys at a
maximum five-second interval computed from exact source cadence; use
`--key-seconds` to select another positive duration. Temporal dependency chains
therefore remain time-bounded rather than changing length arbitrarily with FPS.
It also rejects rates above 30 fps because hardware lower-bound testing proved
that dense full-frame temporal reconstruction cannot meet the 60 fps budget.

The one-command creator defaults to qualified `motion` mode at up to 30 fps.
It evaluates the hardware-proven spatial representations plus a bounded
whole-frame translation residual and selects the motion candidate only when it
removes at least one complete sector. Above 30 fps it defaults to `spatial`.
Use `--video-mode current` for the single-bounding-rectangle path or
`--video-mode spatial` to disable dependencies. Explicit `--video-mode auto`
also adds experimental dense temporal XOR+LZ4.

Motion search memoizes every bounded translation score within a frame. This
does not change candidate ordering or output bytes; on the repeatable 192-frame
creator corpus it reduced normal wall time from 30.951 to 28.295 seconds
(8.6%). A profile attributes most remaining host time to the exhaustive pure-
Python LZ4 match search. A follow-up ten-class quick corpus produced the same
3,591,792 total IPVF bytes with exhaustive `best` and direct HC12, while HC12
reduced conversion time from 27.546 to 10.170 seconds (63.1%). The established
eight-second real-footage sample likewise remained 4,308,000 bytes and strictly
source-valid while falling from 26.809 to 8.453 seconds (68.5%). `balanced` is
therefore the friendly default; exhaustive `best` remains available when every
compressed byte matters more than creator time.

The sector lab now also subtracts the canonical 16-byte record header and uses
each generated clip's actual adaptive audio class: zero payload for silence or
exhausted audio, mono IMA for exact one-channel sources, and stereo IMA
otherwise. Complete record rankings were already sector-based; the corrected
components make boundary and padding reports exact as well.

IPVF requires at least 4 fps. Every stored record is limited to 192 sectors (96
KiB), so it fits the player's dedicated read buffer even when incompressible
video takes the raw fallback. If decoded audio is longer than the converted
video, it is trimmed. If it is shorter, the encoder appends silence so video and
audio have exactly the same duration.

Sources with no audio stream are accepted directly. The creator detects the
absence with FFprobe, skips the FFmpeg audio map, and emits exact zero-payload
silence records while retaining the same 44.1-kHz decoded timeline and audio-
clock contract. No synthetic PCM needs to be stored.

## File layout

All integers are little-endian. The first record begins at byte 512.

| Offset | Size | Meaning |
| ---: | ---: | --- |
| 0 | 4 | `IPVF` |
| 4 | 2 | logical header size: `80` |
| 6 | 2 | width: `220` |
| 8 | 2 | height: `176` |
| 10 | 2 | frame-rate numerator |
| 12 | 2 | frame-rate denominator |
| 14 | 2 | first record size in 512-byte sectors |
| 16 | 4 | frame count |
| 20 | 4 | flags: base `11`; temporal files use `27` (`base | TEMPORAL_XOR`) |
| 24 | 4 | first record offset: `512` |
| 28 | 2 | audio format: `2` (`IMA_ADPCM`) |
| 30 | 2 | channels: `2` |
| 32 | 2 | bits per sample: `16` |
| 34 | 4 | sample rate: `44100` |
| 38 | 4 | total decoded stereo sample frames |
| 42 | 2 | reserved zero |
| 44 | 8 | exclusive media-record end offset |
| 52 | 8 | keyframe-index offset; currently equal to the media end |
| 60 | 4 | keyframe-index entry count |
| 64 | 2 | index entry size: `16` |
| 66 | 2 | metadata TLV byte count |
| 68 | 4 | metadata offset: `80` |
| 72 | 4 | Rockbox CRC32 of all index entries |
| 76 | 4 | media identity: Rockbox CRC32 of all sector-aligned media records |
| 80 | variable | bounded metadata TLVs: one-byte tag, one-byte UTF-8 length, value |
| after metadata | to 512 | zero padding |

Each video frame is one whole-sector record:

| Record offset | Size | Meaning |
| ---: | ---: | --- |
| 0 | 1 | type: key raw `0`, rectangles raw `1`, repeat `2`, key LZ4 `3`, rectangles LZ4 `4`, temporal XOR+LZ4 `5`, translated residual+LZ4 `6` |
| 1 | 1 | rectangle count |
| 2 | 2 | next record size in sectors; zero only for the final frame |
| 4 | 4 | stored video bytes |
| 8 | 4 | decoded video bytes |
| 12 | 4 | stored audio bytes: zero, mono IMA size, or stereo IMA size |
| 16 | variable | raw or independent LZ4 video block |
| after stored video | variable | matching silence, mono IMA, or stereo IMA payload |
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

A type-6 video payload begins with signed one-byte `dx` and `dy` translation
components, followed by a four-byte little-endian Rockbox CRC32 and an
independent raw LZ4 block. The block expands to 77,440 bytes of
`translate(previous_frame, dx, dy) XOR current_frame`; uncovered prediction
pixels are black. The player translates the persistent reference in place with
overlap-safe row moves, verifies and decodes the residual, then performs the
same XOR/render-copy path as type 5. Forced true keys bound reconstruction,
seeking, and resume work.

The keyframe index immediately follows the final sector-aligned media record.
Every 16-byte `<frame_u32, absolute_offset_u64, sectors_u16, flags_u16>` entry
names one true keyframe; flag bit 0 distinguishes an LZ4 key. Entries are
strictly increasing, begin at frame zero, enumerate every keyframe, and are
covered by the superblock's index CRC. Keeping only keyframes makes the index
small: a 45-second real-motion validation file needed 10 entries, or 160 bytes.
Normal playback remains a sequential record walk.

For frame `n`, define the cumulative audio boundary:

```text
S(n) = round(n * 44100 * fps_den / fps_num)
```

For `N = S(n + 1) - S(n)`, stored audio bytes are one of:

- `0` for exact silence;
- `4 + floor(N / 2)` for exact dual-mono IMA;
- `8 + N - 1` for stereo IMA.

Mono uses one four-byte predictor/index header and packs two successive sample
codes per byte; the player duplicates each decoded sample to left and right.
Stereo uses one four-byte header per channel and stores the next left/right
codes in each byte. The header's total decoded audio count remains
`S(frame_count)`, so A/V duration is independently verifiable.

The player accepts only the layout above.

## Device implementation

On PP5020 hardware, the player preserves the qualified three-slot pipeline:

1. The CPU issues one sector-aligned read into a dedicated uncached 96 KiB
   staging buffer. Video and its matching IMA block arrive in that read.
2. The CPU decodes the record's IMA samples into a power-of-two mixer ring
   obtained from Rockbox's plugin audio buffer. Raw video is copied directly to
   an acquired render slot. Compressed true keys expand directly into the
   cached canonical reference and are copied once to the slot. Compressed
   rectangles expand into cached scratch, copy once to the slot, and update the
   reference from cached scratch; full-width updates use one contiguous copy.
   This keeps LZ4 match work away from uncached render memory. LZ4 literals use
   Rockbox's bulk copy from the uncached read buffer; short cached match copies
   stay inline. Temporal residuals are XORed into the cached reference,
   CRC-checked, and bulk-copied once to the slot.
3. The COP sends the prior decoded slot through the target LCD driver while the
   CPU reads and decodes the next record.
4. After the first frame and its audio are decoded and the image is presented,
   the CPU scans future records to prebuffer about four seconds of audio, seeks
   back to the second record, and starts Rockbox's playback mixer at 44.1 kHz.
   Consumed PCM samples become the master clock for later video frames.
5. If storage ever outruns the audio ring, the mixer channel stops and restarts
   when the next record arrives. The audio clock pauses with it, preserving A/V
   alignment instead of silently drifting.
6. Displayed slots are released immediately. At exit, the CPU rereads and
   decodes from the last keyframe, reconstructs the final framebuffer, and
   commits it once so Rockbox resumes with coherent state.
7. Play pauses/resumes the dedicated mixer channel without changing ring
   counters, so decoded audio remains the clock. Left and Right seek ten
   seconds through the keyframe index. The validated raw index is cached in
   spare plugin-buffer memory when it fits, so ordinary runtime binary searches
   issue no index reads; a bounded disk fallback remains. The cache copy is
   independently CRC/order/bounds checked and failure only disables the cache.
   Seeking atomically stops and rebases audio after accepting the request,
   drains and restarts the render worker before fallback storage lookup, and
   reconstructs from the preceding keyframe without
   acquiring render slots or producing throwaway output for intermediate
   frames, promotes the exact target to one full-frame LCD update, prebuffers
   one second from that sample boundary, applies a 256-sample (5.8 ms) fade-in
   to suppress seek-boundary clicks, and restarts the mixer.
   Distinct rapid clicks accumulate instead of collapsing into one jump, and
   clicks received during reconstruction carry into the next target. Seeking
   beyond EOF clamps to the final frame and completes playback normally.
   MENU and normal completion return directly to Rockbox without a diagnostic
   frame-count/status screen; genuine failures retain one short error message.
8. Center Select pauses playback and opens an ownership-safe details screen
   with title, artist, album, duration, frame rate, and file size. Select or
   Play restores the exact paused video frame and resumes; MENU exits.
9. Resume state uses two alternating 36-byte records in Rockbox plugin storage
   per media identity. The prior valid slot survives a torn update. Records
   distinguish active, dismissed, and complete state so completion never
   destructively exposes an older position. Checkpoints use only frames the
   renderer has definitely presented, run every 30 seconds and on pause,
   details, seek completion, clean stop, USB, poweroff, or reboot, and ask
   whether to resume when the same content is opened under any filename.
10. Detected corruption has bounded behavior. A malformed IMA header feeds the
    exact record duration as silence. A failed LZ4 or temporal-CRC decode holds
    the last valid picture until the next successfully decoded true keyframe.
    An invalid optional index does not block sequential playback; seeking uses
    a bounded scan of structural record headers. Broken chains, reads, or the
    first video frame still stop safely, and no recovery path retries forever.

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

They verify the canonical header, metadata TLVs, content-derived media
identity, complete keyframe index and CRC, exact rational cadence,
raw/LZ4/repeat sector chaining, LZ4 roundtrip and malformed-input rejection,
adaptive silence/mono/stereo IMA sizing and predictors, silence padding,
trimming to the
converted video duration, and the 96 KiB record bound.

`validate.py` is the strict streaming file validator. Its format, metadata,
cadence, IMA, LZ4, motion, and FFmpeg reference code lives in `reference.py`
and does not import the production encoder or its decode helpers. It walks sector links,
checks all bounds and zero padding, decodes every LZ4 and IMA block,
reconstructs every video mode, verifies temporal payload and keyframe-index
CRCs, confirms every index entry names the exact keyframe record, and can
compare every frame byte-for-byte with fresh FFmpeg output:

```sh
python3 tools/ipvf/validate.py --source input.mp4 output.ipvf
```

Create the bounded device-recovery control and three deterministic corrupt
cases from one strict-valid temporal file with:

```sh
python3 tools/ipvf/make_recovery_cases.py input.ipvf recovery-cases
```

The generator first requires a strict-valid source, gives every media mutation
a fresh content identity, and confirms that the independent inspector rejects
each malformed output for the intended reason.

Run the broader host malformed-file gate from a short strict-valid IPVF
containing a stored IMA block, a spatial LZ4 record, and ordinary sector
padding:

```sh
python3 tools/ipvf/mutation_corpus.py input.ipvf mutation-results
```

It generates and verifies 34 deterministic failures spanning header fields,
metadata TLVs, bounds and EOF, record links and sizes, superblock/sector
padding, rectangle geometry, LZ4 decoded sizes and offsets, IMA headers, media
identity, and index contents. It writes `host-validation.jsonl` plus a short
`SUMMARY.md`; success means every malformed file was rejected for its intended
reason.

When source-verifying an experimental reduced-color encode, pass its host color
cleanup as well. Everyday and compact both use the validator's RGB565 default:

```sh
python3 tools/ipvf/validate.py --source input.mp4 \
  --color-depth rgb555 output.ipvf
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
audio issue or immediate visible defect. No objective 20 fps quality defect was
established. A later complete RGB444/24 run exposed visible gradient banding
that the short comparison missed. A focused full-file comparison found RGB565
clean, RGB454 noticeably banded, and RGB444 very noticeably banded; everyday
therefore retains RGB565. RGB555 was also noticeable and its 8.40% saving was
not worth the quality loss. Compact instead uses 20 fps with full RGB565. This
23.976-fps source cannot evaluate true 30/60 motion by frame duplication;
native-rate footage and host interpolation need separate tests.
The follow-up viewer adds bounded wheel-event draining and maps clockwise and
counter-clockwise movement to Rockbox's clamped playback volume. A combined
A1099 pass confirmed ordinary track playback, IPVF playback, live IPVF volume,
and uninterrupted audio/video operation.

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

Whole-frame motion prediction subsequently passed its matched real-footage
qualification. The 45.09-second 24000/1001-fps file used 573 translated records
among 1,081 frames and shrank from 27,835,040 to 24,687,264 bytes: 11.3% for
the complete file including unchanged adaptive audio. Strict validation
reconstructed every RGB565 frame exactly. On A1099, the motion file was
indistinguishable from the spatial control during playback, volume changes,
pause/resume, and repeated seeking. Motion is therefore the creator default at
up to 30 fps; dense XOR remains the separate experimental `auto` extension.

The follow-up memory/navigation batch reduced each of the three render-slot
strides from 128 KiB to 96 KiB, recovering 96 KiB of plugin-buffer space while
retaining all three CPU/COP ownership slots. The parser's maximum decoded
record plus header is 81,552 bytes, leaving 16,752 bytes of guard space in each
slot plus a compile-time size assertion. Its exact-cadence test file indexed
frames `0,119,...,1071`; strict source reconstruction passed, and A1099
playback, volume, pause/details, rapid seeking, MENU/reopen, and resume showed
no regression.

The native-rate/silent-source batch used one eight-second native-60 motion
source. Matched output passed strict source reconstruction at 30 fps (240
frames, 3,587,632 bytes, 216 motion records) and 60 fps (480 frames, 7,865,536
bytes, spatial mode). Every record carried zero audio bytes. Both played cleanly
on A1099 with normal silent output and controls; 60 fps felt faster/more
immediate than 30 fps while both retained the same exact eight-second duration.

A separate 24-to-60 motion-interpolation gate was rejected. Matched eight-
second files measured 4,269,600 bytes at source cadence, 4,713,504 bytes for
ordinary duplicated 60 fps, and 10,768,928 bytes for host-interpolated 60 fps.
Duplicated 60 showed slight A1099 stutter, while interpolated 60 stuttered
heavily throughout. The experiment also required endpoint padding and exact
frame trimming to avoid shortening 480 frames to 476. The production creator
retains no interpolation option; native source cadence remains preferred.

Broader qualification still includes longer drift runs, line out, deliberate
storage stalls, USB insertion, and repeat-heavy audio content. Production
playback appends one small teardown-only row to `.rockbox/ipvf-runs.tsv`; the
row contains timing/outcome counters, scheduler `HZ`, initial-start ticks, and
decoded-audio ring capacity/callback count/low-water, but no media filename,
title, or hash. A mixer-empty
callback is counted as a gap only while output is expected, so deliberate seek,
MENU, and teardown stops do not inflate it. The file rotates at 32 KiB and
retains one bounded predecessor. It performs no I/O in the playback loop and
displays no diagnostic frame counters. Full
qualification telemetry remains compile-time opt-in: define
`IPVF_ENABLE_QUALIFICATION_TELEMETRY=1` only for an instrumented build.

A canonical five-minute lifecycle file is prepared for hardware testing:
7,202 RGB565 frames at 24 fps, 61 indexed keys, spatial LZ4 video, adaptive IMA
audio, and 205,264,336 total bytes. Strict validation passes. The creator also
validates all metadata before starting PCM extraction or frame encoding, so an
invalid tag cannot waste a long encode and fail only at header finalization.

The file completed on A1099 with volume, details, pause, rapid seeking,
MENU/resume, and natural completion working. A repeatable 30-second hiccup was
traced to periodic resume slots being reread and truncated on FAT. Resume slots
are now allocated before playback, sequence/slot selection remains in RAM, and
checkpoints overwrite fixed slots in place. A targeted 71-second rerun crossed
the next checkpoint with zero audio gaps, rebuffers, or errors.

The cached spatial-copy reduction then passed its A1099 gate. A seek/control
run had no visible corruption, audible hiccup, rebuffer, or error; its one
mixer-empty callback appeared only in that control-heavy run. A separate untouched
1,148-frame run logged zero late presentations, audio gaps, rebuffers, and
errors. Anonymous rows are retained in
`qualification/2026-09-01-cached-spatial-copy-runs.tsv`.

Off-screen seek reconstruction and the split seek/startup audio cushion passed
their A1099 gate. The first control completed four seeks and reported a
209-tick worst seek while using the four-second cushion. With a one-second seek
cushion, the follow-up completed five seeks with a 111-tick worst result, zero
late frames, rebuffers, or errors, and no visible or audible issue. Startup and
true starvation recovery retain four seconds. Evidence is retained in
`qualification/2026-09-01-seek-reconstruction-runs.tsv`.

The next short device gate adds no codec or buffering-policy change. The mixer
callback records decoded frames remaining whenever it requests its next DMA
block, while excluding expected EOF drain. The teardown journal records the
lowest value, sample count, scheduler `HZ`, and first-start ticks, and deliberate
mixer stops are excluded from the underrun counter under the PCM lock. This
makes a future startup/read-ahead comparison and true starvation directly
measurable without detailed qualification logging.

The exact callback-boundary gate observed 929 mixer refill callbacks, a
zero-frame low-water once during two seeks, and 66 startup ticks at 100 Hz. It
completed with zero late frames, rebuffers, errors, or visible issues. This
confirms both that deliberate stops no longer inflate the counter and that the
one remaining empty callback is a brief real control-heavy boundary worth
retaining in evidence rather than hiding.

A bounded read-ahead path now reuses only the otherwise-unused tail of the
plugin-owned audio allocation. Startup and completed-seek prebuffer scans retain
a contiguous prefix of whole raw records and replay that prefix without reading
it from storage a second time. The decoded-audio ring remains capped at its
qualified 1 MiB, cache access uses the PP5020 uncached alias, and an allocation
with no room for a complete record follows the prior seek-back/reread path.
Teardown journal fields record tail capacity, loads, bytes, records, and replay
hits. Hardware exposed 27,649,164 tail bytes and served 161 records from RAM
across fresh/seek and resume runs. Fresh startup was 68 ticks versus the earlier
66-tick baseline, resume startup was 53 ticks, and both runs had zero late
frames, gaps, rebuffers, recovery events, or errors. Anonymous evidence is in
`qualification/2026-09-01-read-ahead-runs.tsv`.
