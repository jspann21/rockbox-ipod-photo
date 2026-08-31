# IPVF improvement and qualification plan

Last research/implementation pass: 2026-08-31
Code baseline: `main` at `c5b5295cbc`, plus the working-tree deterministic
corpus, compression-lab, adaptive LZ4HC, and production-telemetry changes below
Target: iPod Photo/Color A1099, PP5020, Rockbox, 220x176 RGB565SWAPPED

Detailed 2026-08-31 hardware evidence and pass-by-pass decisions:
`tools/ipvf/qualification/2026-08-31-hardware-runs.md`.

## Purpose

This is the working checklist for turning IPVF from a functioning native
video/audio path into the best practical everyday movie format for the iPod
Photo. It covers file size, high-motion compression, playback performance,
CPU/RAM/storage behavior, seeking/resume, reliability, battery use, and broad
qualification.

IPVF is ours. We do **not** need to preserve an older IPVF layout, version, or
decoder. When evidence supports a cleaner canonical design, replace the old
design rather than carrying compatibility code.

The plan is evidence-driven:

- optimize total sectors read and stored, not an isolated payload byte count;
- keep host encoding as sophisticated as useful when it makes device decoding
  simpler;
- measure on the A1099 before promoting a device-side optimization;
- preserve known-good cache, storage, mixer, COP, and LCD ownership contracts;
- test one bounded question at a time and retain a rollback build;
- keep production free of hot-path logging.

## Current verified baseline

### Format and player

- [x] Native 220x176 RGB565BE video records.
- [x] Raw keyframe, raw rectangle, repeat, LZ4 keyframe, and LZ4 rectangle
      record modes.
- [x] Adaptive CRC-bearing temporal XOR+LZ4 records with bounded keyframe
      dependency chains and a header capability bit.
- [x] Bounded multi-rectangle encoder candidate; the existing player already
      accepts and renders multiple native-aligned rectangles.
- [x] One sector-aligned, bounded read unit containing video plus matching
      audio.
- [x] Maximum stored record: 192 sectors / 96 KiB.
- [x] Stereo 44.1 kHz IMA ADPCM on disk, decoded into Rockbox's PCM mixer.
- [x] Decoded audio consumption is the video clock.
- [x] About one second of audio is prebuffered before playback starts.
- [x] Three-slot CPU/COP pipeline with target-driver-owned LCD updates.
- [x] Exact final-frame reconstruction from the last keyframe at exit.
- [x] MENU stop and USB exit paths exist.
- [ ] User seeking, pause, resume, and persistent playback position.
- [ ] Durable production-quality run journal.
- [ ] Long-duration and lifecycle qualification.

### Current measurements

The final eight-second LZ4/IMA music-video files are:

| Rate | File size | Decimal MB/min | Two-hour projection | Device result |
| --- | ---: | ---: | ---: | --- |
| 30 fps | 4,640,768 B | 34.81 MB/min | 4.18 GB | 240 frames, 0 late, 0 gaps |
| 60 fps | 4,713,984 B | 35.35 MB/min | 4.24 GB | 480 frames, 41 late, 0 gaps |

These are about 51% smaller than the earlier PCM candidates. The 60 fps late
count is an audio-clock boundary metric, not a dropped-frame count; playback
looked and sounded correct.

Important size facts:

- raw RGB565 is 77,440 bytes/frame, approximately 139.4 MB/min at 30 fps and
  278.8 MB/min at 60 fps (132.9 and 265.9 MiB/min);
- current IMA audio is approximately 2.65 MB/min plus small per-record headers;
- therefore video, not audio, dominates current movie size;
- the current eight-second result is one music-video workload, not proof of a
  universal movie bitrate.

Historical high-motion host measurements are directional because they use an
older container parser, but they identify the strongest next candidate:

| High-motion 60 fps video representation | Bytes vs raw |
| --- | ---: |
| Current absolute bounding rectangle before compression | 95.4% |
| Full-frame LZ4 | 26.9% |
| Temporal XOR + LZ4 | 18.8% |
| Temporal XOR + PackBits | 22.8% |
| Sparse 8x8 tiles | 30.3% |

XOR+LZ4 was about 30% smaller than ordinary full-frame LZ4 on that corpus. Its
first device pass confirmed the storage result but rejected the initial decode
implementation on performance grounds. For local-motion 60 fps, the current
rectangle+LZ4 representation beat XOR+LZ4 (1.6% versus 2.1% of raw), so all
future modes must be selected adaptively and score device cost as well as size.

### 2026-08-31 deterministic corpus and host-lab checkpoint

P0.3/P1.1 now have a reproducible WSL pipeline:

- `tools/ipvf/generate_corpus.py` generates lossless seeded sources and a
  hash-bearing manifest. The default FFV1/NUT output is byte-identical across
  reruns; Matroska remains optional but is not canonical because its muxer
  inserts changing identity metadata.
- The standard run contains 18 clips and 718 frames covering static/local/
  global motion, cuts/fades, grain and full noise, odd/even boundaries,
  alternating frames, and silence/mono/stereo/impulse/clipping/short/long
  audio cases.
- `tools/ipvf/lab.py` evaluated 60 complete-record strategies per clip using
  actual IMA sizes and 512-byte rounding. It emits `encode-results.jsonl`,
  `size.csv`, `summary.csv`, `timing.csv`, `pareto.csv`, and provenance.
- Saved evidence is under `dist/ipvf-corpus-p0.3-nut-20260831` and
  `dist/ipvf-lab-p0.3-20260831`.

Aggregate standard-corpus results are:

| Adaptive strategy | Record bytes | Saving vs original current path | Device change |
| --- | ---: | ---: | --- |
| Original current | 6,279,168 | - | none |
| Existing spatial/built-in LZ4 | 6,115,840 | 2.60% | already qualified |
| Best of built-in and official LZ4HC-12 | 5,927,936 | 5.59% | none |
| Spatial + bounded Sub16/LZ4HC candidate | 5,669,376 | 9.71% | predictor inverse required |

Official LZ4HC emits the same raw LZ4 blocks accepted by the current decoder.
The encoder therefore now compares its built-in block with LZ4HC level 12 and
keeps the smaller one per record. Missing host `liblz4` safely falls back to
the built-in encoder. An exact encode/validate check on `global-shake-30`
reduced the complete file from 585,216 to 562,176 bytes (3.94%). Sub16 remains
lab-only until its inverse cost and playback timing pass hardware gates.

### 2026-08-31 real-footage movie-profile checkpoint

`tools/ipvf/profile_lab.py` tested MP4-style host preprocessing on three
five-second scenes from the 224.792-second `suds` real-footage source. It is
1920x820 H.264/YUV420 at 24000/1001 fps with 44.1-kHz stereo AAC. Every
candidate was converted losslessly to a test source, encoded with adaptive
spatial/LZ4HC, strictly source-validated, and compared with the native 24-fps
reference after display-size conversion. Evidence is under
`dist/ipvf-profile-lab-suds-v2-20260831`.

| Host profile | 15-second IPVF | Saving | SSIM vs native 24 | 2-hour projection |
| --- | ---: | ---: | ---: | ---: |
| Native 24 fps | 9,454,592 B | - | 1.0000 | 4.54 GB |
| Native 20 fps | 7,985,664 B | 15.5% | 0.9838 | 3.84 GB |
| Native 15 fps | 6,154,752 B | 34.9% | 0.9628 | 2.96 GB |
| RGB555, 24 fps | 8,790,016 B | 7.0% | 0.9831 | 4.22 GB |
| RGB454, 24 fps | 7,915,008 B | 16.3% | 0.9647 | 3.80 GB |
| RGB444, 24 fps | 7,152,128 B | 24.4% | 0.9562 | 3.44 GB |
| RGB454, 20 fps | 6,702,080 B | 29.1% | 0.9498 | 3.22 GB |
| RGB444, 20 fps | 6,074,880 B | 35.7% | 0.9416 | 2.92 GB |
| Mild/strong denoise, 24 fps | 9.33/9.23 MB | 1.3/2.4% | 0.9985/0.9962 | 4.48/4.43 GB |
| 80% active image, 24 fps | 6,842,368 B | 27.6% | 0.5504 | 3.29 GB |
| Compact 70%/15 fps/RGB444 | 3,062,784 B | 67.6% | 0.5165 | 1.47 GB |

Conclusions are bounded to this real clip but clear enough to guide hardware
A/B tests. Native 20 fps is a compact size/quality option, not the quality
default. Source-native 24 fps preserves this clip's original motion cadence.
RGB454/24 and RGB444/24 are useful medium/high compression candidates. Denoise
alone is too small a gain; adding it to RGB444 saved only another 0.7%. Reduced
active resolution causes much greater measured damage than color quantization
for similar storage, so it should be an explicit compact profile, not default.
Frame-rate SSIM understates motion/judder perception; actual LCD A/B remains a
promotion requirement.

### 2026-08-31 A1099 lossy-profile LCD checkpoint

The production 13,924-byte viewer and five randomized-label profiles were
installed and their copied contents were verified. The compared profiles were
native-24, native-20, RGB454/24, RGB444/24, and RGB444/20. No immediate visual
defect was apparent; all were smooth with no audio or playback issue. A
follow-up subjective preference favored the motion feel of 30 fps, or possibly
60 fps, over 20 fps. RGB444/20 therefore passes only as a compact profile,
reducing complete IPVF size by 35.7% versus native-24. RGB444/24 is the leading
tested reduction that preserves this source's cadence, at 24.4%. Broader
material must still test animation, text, dark gradients, grain, rapid camera
motion, and duration.

This source is 24000/1001 fps, so encoding it at 30 or 60 fps without motion
interpolation would mostly duplicate frames and would not test true higher-rate
motion. A separate gate needs native 30- and 60-fps real footage. Host motion
interpolation from 24 to 30/60 is a separate lossy experiment whose artifacts,
storage, and device workload must be measured.

The same pass exposed a separate usability gap: volume could not be adjusted
in any clip. The player currently consumes buttons once per frame but handles
only MENU stop and USB; it has no wheel-to-volume mapping. This is not a codec
or audio-decoder failure, but normal volume control is required before the
viewer is considered complete.

### 2026-08-31 adaptive temporal checkpoint

The encoder now compares current bounding-rectangle/LZ4, bounded
multi-rectangle/LZ4, and previous-frame XOR+LZ4 candidates using the complete
sector-rounded record cost including audio. A more expensive mode is selected
only when it removes at least one whole 512-byte sector. Temporal records carry
the Rockbox non-reflected CRC32 of the reconstructed frame and true keys remain
forced every 120 frames.

Every file below passed strict record/link/padding validation, full IMA block
decoding, per-temporal-record CRC verification, and byte-for-byte comparison
of every reconstructed frame with fresh FFmpeg RGB565BE output:

| Corpus/rate | Current bytes | Adaptive bytes | Whole-file saving | Final CRC |
| --- | ---: | ---: | ---: | --- |
| high motion, 30 fps | 4,815,360 | 3,940,864 | 18.2% | `5b2183e9` |
| high motion, 60 fps | 9,285,632 | 6,637,568 | 28.5% | `d9a00934` |
| music, 30 fps | 4,640,768 | 4,263,424 | 8.1% | `af3267df` |
| music, 60 fps | 4,713,984 | 4,343,808 | 7.9% | `af3267df` |
| local motion, 30 fps | 897,024 | 872,448 | 2.7% | `97f4a351` |
| local motion, 60 fps | 1,024,512 | 964,608 | 5.8% | `97f4a351` |

The WSL `ipodcolor` build `dddffd0151M-260831` compiled and linked cleanly.
The first 12-file device pass is archived under
`dist/ipvf-qualification-results-20260831`; the prior complete `.rockbox` tree
is preserved under `dist/ipod-before-ipvf-xor-20260831`.

### 2026-08-31 first A1099 temporal result

All 12 files completed with the declared frame count, zero format/decode
errors, zero render failures, and the expected final framebuffer CRC. The
system snapshot also had zero DMA timeouts/fallbacks/IRQ anomalies, zero PCM
underruns, and zero timeout registration failures. This proves the temporal
format, dependency tracking, CRC, audio interleave, and lifecycle path.

The initial decoder did not pass its real-time gate:

| Corpus/rate | Current -> XOR gaps | Current -> XOR video decode ms/frame | XOR peak decode |
| --- | ---: | ---: | ---: |
| high motion, 30 fps | 0 -> 47 | 13.73 -> 37.87 | 42.11 ms |
| high motion, 60 fps | 45 -> 140 | 13.72 -> 40.14 | 42.22 ms |
| music, 30 fps | 0 -> 24 | 11.43 -> 24.20 | 46.49 ms |
| music, 60 fps | 1 -> 52 | 5.73 -> 12.05 | 46.40 ms |
| local motion, 30 fps | 0 -> 0 | 0.96 -> 1.87 | 34.46 ms |
| local motion, 60 fps | 0 -> 10 | 0.56 -> 1.56 | 34.75 ms |

Read and LCD/render timing stayed comparable; the added cost is video decode
and reconstruction. Temporal `auto` remains an explicit experiment.

The second installed pass tests two follow-ups together:

- spatial-only multi-rectangle files, which save 9.1%/11.4% on high-motion
  30/60 fps without temporal dependencies;
- decoder revision `xor-fastcrc-1`, which fuses XOR and CRC into one cached
  pass using a generated byte table and logs LZ4, reconstruct, and copy time
  separately.

The pass-2 package is `dist/package-ipvf-qualification-v2-20260831` and its
numbered files are installed under `Videos/IPVF Qualification 2`.

### 2026-08-31 spatial promotion and temporal V2 result

The second device pass completed all 12 files with matching final CRCs and no
decoder/render errors. Spatial mode is promoted as the encoder default:

- high-motion 30 fps saved 9.1%, reduced decode from 13.73 to 11.63 ms/frame,
  reduced render from 11.18 to 8.95 ms/frame, and retained zero gaps;
- high-motion 60 fps saved 11.4% and cut gaps from 45 to 7;
- local-motion saved 2.4%/5.0% at 30/60 fps with zero gaps;
- music files stayed the same size and timing, with zero gaps;
- no spatial candidate expanded a file because selection is sector-aware.

Fused byte-table XOR/CRC reduced dense temporal decode from about 38--40 to
25 ms/frame. High-motion 30 fps improved from 47 to zero gaps, but temporal
reconstruction still cost about 13.8 ms per XOR frame. High-motion 60 fps
remained at 140 gaps, and music 60 fps visibly stuttered with 55 gaps.

Pass 3 showed that slicing-by-four is a regression on the PP5020: temporal
reconstruction increased from about 13.8 to 15.7 ms per XOR record. Music 60
fps still visibly stuttered with 44 gaps and high-motion 60 fps logged 120.
The implementation is rejected and removed.

Pass 4 is a bounded lower-bound experiment. Decoder revision
`xor-nocrc-bound-3` performs aligned word XOR without checking the full-frame
CRC on-device; the file still stores the CRC and the strict host validator
still checks every frame. This diagnostic result will decide whether to design
a cheaper integrity field or stop investing in full-frame temporal XOR. Its
five-file package is `dist/package-ipvf-qualification-v4-20260831`.

Pass 4 completed with exact output and zero decoder/render errors. Plain XOR
cost 6.13--6.14 ms per dependent frame. High-motion 60 fps still averaged
17.99 ms total decode and logged 95 gaps; music 60 fps visibly stuttered with
27 gaps. Both dense 30 fps workloads reached zero gaps. Full-frame temporal
XOR is therefore capped at 30 fps, while 60 fps remains spatial.

Pass 5 replaces the reconstructed-frame CRC with a CRC over the smaller
compressed residual. The player checks it before LZ4 decoding, preserving
stored/read corruption detection without a second full-frame pass. Decoder
revision `xor-payloadcrc-4` and three regenerated 30 fps files are installed;
the package is `dist/package-ipvf-qualification-v5-20260831`.

Pass 5 completed successfully. All three 30 fps files had exact final CRCs,
zero decoder/render errors, and zero audio gaps; the music file looked and
sounded correct. Payload CRC cost 0.34--5.96 ms per temporal record and aligned
XOR about 6.22 ms. This is the checked temporal implementation, while spatial
remains the default until long-duration qualification is complete. Device test
material and logging were removed after evidence collection.

### Known hardware boundaries to preserve

- [x] A full LCD update is 19,360 status-gated 32-bit writes.
- [x] Stock and safely optimized feeders measured 11.651 ms and 11.645 ms.
- [x] The LCD path is interface-bound; another ARM loop tweak is not a useful
      target.
- [x] Keep TXOK checking before every two-pixel LCD data word.
- [x] Keep LCD programming inside the target driver.
- [x] No documented PP5020 LCD DMA request/completion contract has been found.
- [x] Direct plugin/COP LCD MMIO, guessed LCD DMA, and unchecked FIFO bursts are
      rejected.
- [x] Keep LZ4 decode in cached scratch, bulk-copy uncached literals, and copy
      decoded output once into the uncached render slot. Earlier byte-at-a-time
      uncached work stuttered.
- [x] Keep the three-slot pipeline unless a complete hardware regression proves
      a replacement. Two slots previously exposed ownership serialization.
- [x] Do not reuse global `apps/buffering.c` as a private plugin stream. If
      read-ahead becomes necessary, make it IPVF-owned.
- [x] Do not revive unproven PP5020 range-cache shortcuts without corruption,
      USB, audio, and concurrent-cache qualification.

## Success scorecard

Every candidate report must include all of the following. A result without the
full scorecard is exploratory, not promotable.

### Size

- total file bytes and decimal MB/min;
- raw video, stored video, audio, header/index, and padding bytes separately;
- complete sector-rounded record bytes;
- median, p95, and maximum record sectors;
- mode counts and keyframe interval;
- two-hour projection and number of required file segments.

### Correctness and quality

- exact frame-by-frame RGB565 reconstruction for lossless modes;
- exact decoded audio-frame count and channel order;
- final framebuffer CRC and selected segment CRCs;
- for lossy modes: PSNR/SSIM plus visual A/B on the actual LCD; metrics do not
  replace visual acceptance;
- malformed input must fail safely with a bounded error path.

### Device performance

- read, video decode/reconstruct, IMA/other audio decode, render queue wait,
  LCD, and framebuffer reconciliation time;
- p50/p95/p99/max, not only totals;
- frames decoded, presented, intentionally skipped, and late as separate
  counters;
- audio underruns/restarts, ring low-water mark, and maximum A/V timing error;
- wall time, CPU frequency/boost state, storage medium, and fragmentation state;
- RAM high-water mark, plugin/audio buffer allocation, and stack canaries.

### Promotion gates

- [x] Host output validates and reconstructs exactly when the
      candidate is lossless.
- [x] Candidate never exceeds the 192-sector record bound.
- [x] Adaptive selection prevents file-size expansion on non-target content.
- [x] A new lossless video mode saves at least 10% total bytes on its target
      high-motion class before paying for device complexity.
- [ ] 30 fps: zero audio gaps, zero dropped frames, no visible defect, no fatal
      error, and no regression from current timing.
- [ ] 60 fps: zero audio gaps/dropped frames, no visible defect, and no worse
      lateness than the identical current baseline.
- [ ] MENU, USB, replay, and final framebuffer restoration pass after every
      device experiment.
- [ ] Battery/power cost is measured before a CPU-heavier mode becomes the
      default.

## Phase 0 - Freeze a reproducible baseline and automate evidence

Goal: make hundreds of host cases and repeated device runs comparable while
only the physical click/eject/reconnect actions remain manual.

### Reproducibility

- [x] Record the exact command and compressor that produced the existing files
      named `suds-lz4hc-ima-*`; current `encode.py` has no `--lz4hc` option.
- [x] Create a corpus manifest with `clip_id`, deterministic source identity,
      duration, source FPS, target FPS, audio properties, motion class,
      encoder settings, and
      expected frame/audio hashes.
- [ ] Pin and report FFmpeg, Python, compiler/toolchain, Git commit, build
      configuration, and produced artifact hashes. Host-lab provenance now
      records FFmpeg, Python, liblz4, Git, manifest hash, and run settings;
      compiler/build artifact capture remains.
- [x] Run all builds and host analysis inside WSL. Use Windows/PowerShell only
      for safely detecting, installing to, and collecting from the mounted
      iPod.
- [x] Preserve the current working firmware/plugin and current 30/60 fps files
      as immutable rollback/baseline artifacts.

### Independent host inspector

- [ ] Add a standalone `inspect/validate` path that does not call the production
      encoder's decode helpers.
- [x] Walk every record, verify sizes/offsets/padding/EOF, decode every mode,
      reconstruct every frame, decode all audio, and compare with the corpus
      manifest.
- [ ] Emit machine-readable `encode-results.jsonl`, `size.csv`,
      `host-validation.jsonl`, and one Markdown summary. The lab now emits the
      first two plus `summary.csv`, `timing.csv`, `pareto.csv`, and provenance;
      validator JSONL and generated Markdown remain.
- [ ] Add deterministic mutation/fuzz inputs for headers, record sizes,
      rectangles, LZ4 lengths/offsets, IMA headers, truncation, padding, and EOF.
- [x] Keep the existing small unit suite; expand broad testing through the
      corpus/validator rather than creating a second format implementation.

### Corpus

- [x] Static: solid, gradient, still photo, slideshow.
- [x] Local motion: one object, two disjoint objects, subtitles, sprites.
- [ ] Global motion: pan, scroll, and camera shake are covered; zoom remains.
- [ ] High motion: sports, water, foliage, crowds, film grain.
- [x] Worst cases: seeded noise, full-screen cuts, flashes, fades, alternating
      frames, one-pixel and odd/even boundary changes.
- [ ] Source shapes: 4:3, widescreen, vertical, letterboxed, 23.976/24/25/30/60,
      VFR, and interlaced sources.
- [x] Audio: silence, mono, stereo ID, impulses, tones, clipping, audio shorter
      and longer than video.
- [ ] Long: 5 minutes, 30 minutes, one hour, and two hours.

### Qualification telemetry

- [x] Compile-gate qualification telemetry; enabled qualification builds still
      require `.rockbox/ipvf-qualification.enable` before writing a TSV.
- [x] In qualification mode, accumulate counters in RAM and append one bounded
      completed TSV row only after playback teardown.
- [ ] Include run/build/clip IDs, record offset, last frame, stage/error code,
      mode counts, stage timing histograms, ring low-water, gaps, lateness,
      final audio count, final framebuffer CRC, battery voltage, and stop reason.
- [ ] Continue updating the retained crash record at meaningful stages so a
      reset before file logging is still diagnosable.
- [x] Avoid per-frame `write()`/CSV logging in the hot path.
- [x] Compile qualification telemetry out by default. The production plugin
      contains no TSV/marker strings, timer reads, 64-bit timing accumulation,
      record accounting, or final framebuffer CRC scan; an explicit build
      macro restores the complete qualification path. The integrated
      production plugin is 13,924 bytes versus 17,080 bytes for the v5
      qualification plugin.

### Install/run/collect loop

- [ ] One WSL command builds, encodes, validates, packages, and writes a test
      manifest/instruction sheet.
- [x] Installer verifies `.rockbox`, target `ipodcolor`, free
      space, and hashes; it backs up replaced files and copies only an allowlist.
- [x] User receives a numbered list of files/actions and only clicks hardware.
- [ ] Collector finds the remounted device, pulls logs/crash state/build info,
      hashes them, and runs the WSL analyzer automatically.
- [ ] Never delete device material until its exact resolved path and manifest
      ownership are verified.

## Phase 1 - Sector-accurate offline compression laboratory

Goal: harvest encoder-only wins first. These can improve file size without
changing the proven player.

- [x] Make one candidate selector evaluate the complete record cost including
      12-byte header, audio, 512-byte rounding, and 192-sector limit.
- [x] Preserve raw fallback and choose the smallest valid sector count per
      record; use byte count and predicted decode cost as tie-breakers.
- [ ] Sweep time-based and scene-cut-aware keyframe intervals. Replace the
      frame-count-only default with a maximum seek/dependency time.
- [x] Compare the current 32-candidate LZ4 search with official LZ4HC levels,
      `--best`, optimal parsing, and decode-speed-favoring output. All use the
      same block decoder. Fast and HC levels 3/9/12 were measured; HC12 is now
      an adaptive host-only encoder candidate.
- [x] Try RGB565 byte-plane split, 16/32-bit word ordering, XOR, horizontal
      `Sub`, vertical `Up`, and simple Paeth-like reversible predictors before
      LZ4.
- [x] Try bounded multi-rectangle segmentation: connected changed tiles and
      runs, and greedy rectangle merging capped initially at 4/8 rectangles.
- [x] Score rectangle candidates on both sectors and LCD calls/pixels; extra
      rectangles can save storage while costing display setup time.
- [ ] Compare 8x8, 16x8, and 16x16 maps with raw, solid-color, palette, XOR, and
      LZ4 tile payloads.
- [ ] Add compact repeat runs and silence runs to measure sector-padding savings
      on animation/static material. Do not assume grouping is important until
      measured; padding is not the current music-video bottleneck.
- [ ] Evaluate per-record palettes/RGB332 only for content classes where the
      selector proves a win.
- [x] Publish a Pareto table: total sectors versus estimated device operations.
- [ ] Reject standalone PackBits for natural high-motion video unless a current
      sector-accurate corpus reverses the existing loss to LZ4.

Decision gate:

- [ ] Select a small set of modes that wins across distinct classes; do not put
      every explored codec into the device decoder.

## Phase 2 - Temporal XOR+LZ4 hardware experiment

Goal: qualify the strongest measured lossless high-motion candidate.

### Design

- [x] Add a full-frame `previous XOR current` residual candidate with ordinary
      key/raw/LZ4 fallback.
- [x] Reset dependency at forced keyframes; scene-cut-aware key insertion is
      still pending.
- [ ] Store an explicit base/dependency distance so corruption and seeking are
      bounded.
- [x] Maintain one persistent 77,440-byte decoded reference frame.
- [x] Decode LZ4 residual into cached scratch, XOR/reconstruct in cached memory,
      then copy once to the acquired uncached render slot.
- [x] Keep audio decode before video work and preserve the one-read record unit.
- [x] Do not remove a render slot to obtain the reference buffer.

### RAM enabling experiment

- [ ] Canary-test a smaller sector-aligned render-slot stride. Current stride is
      128 KiB although the largest decoded payload plus header needs about
      81,920 bytes when rounded to 512.
- [ ] Test 81,920-byte and conservative 96-KiB strides with all three slots,
      full-screen keyframes, maximum rectangle payload, COP display, replay,
      MENU, USB, and cache stress.
- [ ] Retain 128 KiB immediately if any aliasing, overlap, corruption, timing,
      or lifecycle uncertainty appears.
- [ ] Use recovered space for the reference frame or an IPVF-owned read buffer,
      not speculative extra features.

### Device matrix

- [x] Run the first current-LZ4 versus XOR+LZ4 matrix at 30 and 60 fps on
      local, music, and high-motion clips.
- [ ] Extend that matrix to pan/scroll, cuts, grain, and seeded noise.
- [ ] Log residual decode, XOR/reconstruction, slot copy, queue, LCD, ring
      low-water, late, and gaps separately. V2 has stage totals/maxima; ring
      low-water and distributions remain.
- [x] Confirm every final CRC and normal lifecycle exit in the first matrix.
- [ ] Accept only if sector savings survive current-format overhead and device
      timing/power gates.

## Phase 3 - Indexed container, seek, pause, and resume

Goal: make IPVF navigable and recoverable without sacrificing sequential reads.
Do this after the Phase 1/2 mode set is known so the canonical layout is changed
once, not repeatedly.

### First settle large-file reality

- [x] Verify source: normal Rockbox builds use `FILE_SIZE_MAX = 0x7fffffff`;
      current file I/O clamps offsets at that limit.
- [ ] Create sparse/real boundary probes around 2 GiB and 4 GiB and test
      `filesize`, `lseek`, reads, EOF, USB, and FAT behavior on the actual build.
- [ ] Choose one:
  - [ ] widen and qualify Rockbox file offsets end-to-end; or
  - [ ] prefer transparent IPVF segmentation below approximately 1.9 GiB.
- [ ] Remember FAT32's own single-file limit; a wider Rockbox offset alone is
      not a universal long-movie solution.

### Canonical layout

- [ ] One 512-byte superblock with rational FPS, 64-bit logical frame/audio
      counts, data/index locations, file/segment identity, and checksums.
- [ ] Sector-aligned media records retain bounded video+audio reads.
- [ ] Store the current record's size, mode, frame number, audio start/count,
      dependency/keyframe state, stored/decoded sizes, and bounded checksums.
- [ ] Replace the current next-record-size chain as the only navigation source.
- [ ] Add a paged frame index. A compact 8-byte entry costs about 1.73 MiB/hour
      at 60 fps and need not be resident; cache one 4-KiB page.
- [ ] Include a compact keyframe/segment table for fast coarse navigation and
      recovery.
- [ ] Measure CRC/checksum CPU cost. Prefer header plus stored-payload checking
      that can be fused with copying/decoding; do not spend the frame budget on
      redundant hashes without evidence.
- [ ] Define deterministic recovery: bad audio becomes timed silence; bad delta
      holds the last valid image until the next keyframe; bad index falls back
      to a bounded sync scan; no infinite retries.

### Seeking and resume

- [ ] On seek: stop mixer, flush queues/ring, find prior keyframe, decode forward
      without LCD presentation, prebuffer audio at the exact target sample,
      present target frame, restart the mixer clock.
- [ ] Test 0/10/50/90/99%, immediately before/after keyframes, rapid repeated
      seeks, and segment boundaries.
- [ ] Add wheel/button controls for pause, seek, restart, and resume choice.
- [ ] Add live playback volume control using Rockbox's existing sound/volume
      state, with bounded wheel-repeat handling and an on-screen level splash.
- [ ] Verify minimum/maximum clamping, rapid wheel input, MENU interaction,
      USB interruption, and that changing volume causes no audio gaps.
- [ ] Persist resume atomically under Rockbox data storage, keyed by media ID,
      with segment/frame/audio sample and a validity checksum.
- [ ] Checkpoint resume sparingly (for example every 30 seconds and on clean
      stop) to avoid hot-path storage writes.
- [ ] Resume correctly after MENU, reboot, USB, and an interrupted prior run.

## Phase 4 - Native temporal/movie codec experiments

Goal: move beyond lossless residual compression if two-hour everyday movies
still exceed the desired storage budget.

### MP4 lessons translated into bounded IPVF work

- [x] Variable bitrate equivalent: choose every record adaptively by complete
      sector cost instead of assigning a fixed byte budget.
- [ ] Chroma reduction: compare YUV420 or host chroma smoothing only after a
      bounded device conversion benchmark; RGB444/454 preprocessing is the
      current zero-decoder-cost approximation.
- [ ] Motion prediction: begin with whole-frame translations, then bounded
      block vectors and residuals; keep one previous reference only.
- [ ] Transform/quantization: test whether a tiny reversible or deliberately
      quantized residual transform beats direct RGB565 Sub/XOR without adding
      an expensive inverse transform.
- [ ] Entropy coding: evaluate stronger host parsing and small ARM-decodable
      coders, but retain LZ4 unless both sector size and device time improve.
- [ ] GOP/keyframe policy: express dependency limits in time, add scene-cut
      keys and an index, and never use unbounded prediction chains.
- [x] Frame-rate profiles: real 24/20/15-fps footage was measured by complete
      IPVF size; preserve source cadence up to 30 fps for the everyday profile,
      use 20 fps for compact output, and treat 15 fps as the stronger motion
      tradeoff.
- [ ] Quality remains an LCD decision: SSIM/PSNR rank candidates but cannot
      approve judder, banding, small text, faces, fades, or dark gradients.

### Low-complexity temporal prediction

- [ ] Start with whole-frame even-pixel translations for pans/scrolling.
- [ ] Then compare bounded integer 16x16, 16x8, and 8x8 motion vectors against
      one previous reference frame.
- [ ] Store skip/copy vectors plus optional XOR/residual literals; keep decoder
      operations to bounded copies, XOR/add, and LZ4.
- [ ] Use expensive offline motion search freely; device decode must remain
      deterministic and simple.
- [ ] Reset at scene cuts/keyframes; never allow an unbounded dependency chain.
- [ ] Test overlap-safe block copies, reference corruption, catch-up, seek, and
      frame skipping explicitly.

### Lossy movie profiles

- [ ] Add rational 23.976/24/25 fps output and compare with 30/60. Integer
      24/20/15 testing is complete and showed 15.5%/34.9% savings at 20/15;
      exact rational container timing remains.
- [ ] Test native 30- and 60-fps real footage rather than judging those rates
      from duplicated 23.976-fps frames; compare motion feel, CPU margin,
      battery, late frames, and complete sector-rounded size on the A1099.
- [ ] Separately test bounded host motion interpolation from 23.976/24 to
      30/60 fps. Reject it if visible interpolation artifacts or resulting
      IPVF size/device cost outweigh the perceived-motion benefit.
- [x] Compare host-side RGB565 quantization that suppresses visually irrelevant
      low-bit changes before residual/LZ4 coding. RGB555/454/444 at 24 fps saved
      7.0%/16.3%/24.4% on the real-footage sample.
- [ ] Compare palette/index modes for animation, UI captures, and cartoons.
- [x] Compare lower active image resolutions letterboxed in the native frame.
      An 80% image saved 27.6% but SSIM fell to 0.5504; keep this out of the
      everyday default pending LCD review.
- [ ] Compare YUV420 only with a device conversion benchmark. Existing host
      results show modest high-motion residual-size improvement, while device
      YUV-to-RGB cost could consume the small 60-fps CPU budget.
- [ ] Define quality levels by measured size and LCD A/B results, not labels
      such as “high” or “low.”
- [x] Compare mild/strong spatial-temporal denoise before LZ4. It preserved
      SSIM at 0.9985/0.9962 but saved only 1.3%/2.4%, so it is not a priority by
      itself.

### Deprioritized codec families

- [ ] Benchmark Heatshrink, LZSA, LZO/NRV, static Huffman/Rice, or a tightly
      bounded small-window Zstd only in the host lab; promote only after they
      beat LZ4/XOR on both sectors and an ARM decoder microbenchmark.
- [ ] Do not begin with Zstd/Brotli/Opus/H.264-class complexity. Better desktop
      ratios do not establish PP5020 RAM, cache, or deadline fitness.
- [ ] Do not revisit MJPEG/general MPEG decode paths without a new fact that
      changes the previously measured decode-cost boundary.

## Phase 5 - Audio size and quality

Goal: improve long-movie audio after video dominates less, without weakening
the working audio clock.

- [x] Keep stereo IMA as the robust baseline: low CPU, tiny state, independently
      decodable records, zero gaps in current qualification.
- [ ] Add a zero-payload silence mode and optional mono IMA profile.
- [ ] Host bakeoff: current IMA, mono IMA, MP2 96/128/160 kbps, MP3 96/128,
      and bounded delta/Rice with raw fallback.
- [ ] Report whole-IPVF savings, not only audio ratio. Replacing current IMA
      with 128-kbps MP2 is only about a 5% total reduction on the present
      eight-second files.
- [ ] Correct premise: Rockbox's libmad path supports MPEG Layer II as well as
      Layer III, but the IPVF plugin cannot simply invoke `mpa.codec`; an MP2
      experiment needs a bounded private wrapper/extraction and RAM accounting.
- [ ] Prototype MP2 at 128 kbps first; measure decoder time, code/RAM, priming,
      seek pre-roll, ring low-water, gaps, late frames, and battery.
- [ ] Use impulse+flash and left/right ID clips to measure physical line-out to
      LCD synchronization, not only the software mixer clock.
- [ ] Do not assume 22.05 kHz hardware output: the current target advertises
      44.1 kHz. Probe the driver/hardware separately or measure resampling cost.
- [ ] Do not move audio decode to the COP while it owns display unless profiling
      proves audio CPU contention and a safe schedule exists.

Reference bitrates:

| Audio mode | Approximate storage/min |
| --- | ---: |
| Current stereo IMA | 2.65 MB |
| Mono IMA at 44.1 kHz | 1.32 MB |
| MP2 96 kbps | 0.72 MB |
| MP2 128 kbps | 0.96 MB |
| MP2 160 kbps | 1.20 MB |

## Phase 6 - Performance, RAM, storage, and battery

- [ ] Profile current IMA and LZ4 separately before hand-optimizing either.
- [ ] Compare current CPU boost with normal/controlled boost on identical files.
- [ ] Compare battery voltage/load traces for 24/30/60 fps, raw/LZ4/XOR, IMA/MP2,
      backlight levels, and headphones/line out.
- [ ] Do not claim temperature without an external sensor; the current model is
      voltage-based and has no coulomb/current/temperature measurement.
- [ ] Test HDD and iFlash, contiguous and deliberately fragmented files,
      near-full storage, and repeated cold/warm reads.
- [ ] Instrument requested `rb->read()` versus actual ATA request/DMA behavior;
      one plugin read does not prove one physical DMA transfer.
- [ ] Keep storage spindown disabled for the baseline, then test bounded
      prefetch/spindown policies for low-bitrate movies.
- [ ] Add an IPVF-owned read-ahead queue only if measured stalls/fragmentation
      starve the current one-record path. Use MPEGPlayer's private disk buffer as
      architectural reference, not the global playback buffer.
- [ ] Measure whether startup's audio prebuffer scan/reread should be replaced by
      an index-assisted or multi-record read.
- [ ] Profile frame catch-up under injected stalls: current pause, timed silence,
      decode-without-display, and keyframe jump. Never call a late frame dropped.

## Phase 7 - Long-duration and lifecycle qualification

### Duration and drift

- [ ] 5-minute smoke for every promoted mode.
- [ ] 30-minute drift/endurance run.
- [ ] One-hour run across the 2-GiB boundary plan.
- [ ] Two-hour movie/loop with segment rollover where required.
- [ ] Verify final consumed audio count, wall time, ring behavior, counter wrap,
      maximum A/V offset, EOF drain, and final framebuffer.

### Interruption and repeated lifecycle

- [ ] Early/middle/late MENU stop followed by immediate replay.
- [ ] Early/middle/late USB insertion during read, decode, audio, and display.
- [ ] Pause/resume and rapid seek/restart once controls exist.
- [ ] Ten full play/stop/reopen cycles.
- [ ] One hundred short cycles for leaks, stuck mixer channels, slot ownership,
      CPU boost, backlight, spindown, and plugin-buffer restoration.
- [ ] Deliberate storage stalls and read errors.
- [ ] Low-battery shutdown and next-boot resume/recovery.
- [ ] Headphones and line out, silence and repeat-heavy audio.
- [ ] Reboot/crash-record inspection after any failed run; do not rerun first and
      overwrite the evidence.

### Release gate

- [ ] Host corpus and malformed corpus pass.
- [ ] 30 fps general-movie corpus passes with zero gaps/drops.
- [ ] 60 fps support is labeled by measured content/profile capability.
- [ ] Seeking/resume and segment rollover pass their full matrix.
- [ ] Long drift, lifecycle, storage, and battery reports are complete.
- [ ] Qualification logging is compiled out and production timing/file size are
      rechecked.
- [ ] README, development history, encoder help, creator workflow, and this plan
      match the code.
- [ ] Friendly command remains one step:
      `python3 tools/ipvf/encode.py input-anything.mp4 output.ipvf`.

## Immediate next batch

Do these in order:

- [x] P0.1: capture reproducible current 30/60 LZ4/IMA baselines and exact
      compressor provenance.
- [x] P0.2: build the strict streaming inspector and sector-accurate size
      report. Independent external-LZ4 decoding remains a hardening item.
- [x] P0.3: generate the deterministic corpus and bulk host matrix.
- [x] P1.1: run current LZ4, official LZ4HC/optimal, predictors,
      multi-rectangle, tiles, and current-format XOR+LZ4 offline.
- [x] P1.2: choose modes by total sectors and decoder-operation budget.
- [ ] P2.1: canary-qualify a smaller three-slot stride without changing the
      pipeline's ownership model.
- [x] P2.2: build one bounded XOR+LZ4 qualification player with sparse logging.
- [x] P2.3: install via the standard WSL package/Windows device loop; run 30 fps
      first, then 60 fps; pull and analyze logs after each gate.
- [ ] P1.3: add one bounded Sub16+LZ4 record candidate and measure its inverse
      loop before deciding whether the extra 4.1% aggregate saving over
      adaptive LZ4HC justifies an A1099 playback matrix.
- [x] P4.1: create matched native-24, native-20, RGB454-24, RGB444-24, and
      RGB444-20 device files from the same real scenes and perform an A1099 LCD
      A/B. All were smooth with no immediate visible defect, but subjective
      motion preference keeps 20 fps compact-only. RGB444/24 preserves cadence while
      saving 24.4% on this sample.
- [ ] P4.2: after LCD selection, expose friendly named creator profiles and
      encode/validate the complete `suds` source, reporting true
      whole-video size rather than only the three-scene projection.
- [ ] P2.4: implement and device-qualify volume control; the current playback
      loop recognizes MENU/USB but ignores wheel volume input.
- [ ] P4.3: run matched native-30/native-60 real-motion files and a separate
      24-to-60 host-interpolated candidate before defining high-rate profiles.
- [ ] P3.1: settle 2-GiB behavior before designing the final index/segment rules.
- [ ] Update this checklist after every host batch and hardware result.

## Evidence and references

### Local source and measurements

- `tools/ipvf/encode.py` - current encoder, one bounding rectangle, LZ4, IMA.
- `tools/ipvf/README.md` - canonical layout and current device results.
- `tools/ipvf/DEVELOPMENT_HISTORY.md` - measured architecture history.
- `apps/plugins/ipodnative.c` and `ipodnative_*.inc` - parser, decoder, mixer,
  player, and CPU/COP pipeline.
- `results/firmware-research/codec-eval-verified.txt` - prior codec comparisons.
- `results/firmware-research/probe_lcd.py` - local RetailOS LCD-object probes.
- `results/firmware-research/ipvf17-a1099-2026-08-28.csv` and `dist/ipvfnative*`
  logs - historical device evidence.
- `firmware/include/fs_defines.h` and `firmware/common/file.c` - practical file
  offset ceiling.
- `firmware/target/arm/ipod/lcd-color_nano.c` - target LCD contract.

### Primary online references

- LZ4 block format and optimal-parsing freedom:
  <https://github.com/lz4/lz4/blob/dev/doc/lz4_Block_format.md>
- LZ4 block independence, dictionaries, and checksums:
  <https://github.com/lz4/lz4/blob/dev/doc/lz4_Frame_format.md>
- LZ4HC/decode-speed options:
  <https://github.com/lz4/lz4/blob/dev/programs/lz4.1.md>
- PNG reversible predictor/filter model:
  <https://www.w3.org/TR/PNG-Filters.html>
- Heatshrink embedded LZSS implementation:
  <https://github.com/atomicobject/heatshrink>
- LZSA formats/implementations:
  <https://github.com/emmanuel-marty/lzsa>
- Zstandard memory-constrained guidance:
  <https://github.com/facebook/zstd/wiki/Using-libzstd-in-a-memory-constrained-environment>
- Zstandard format: <https://www.rfc-editor.org/rfc/rfc8878>
- DEFLATE format: <https://www.rfc-editor.org/rfc/rfc1951>
- Opus specification: <https://www.rfc-editor.org/rfc/rfc6716>
- Rockbox libmad Layer I/II implementation:
  <https://github.com/Rockbox/rockbox/blob/master/lib/rbcodec/codecs/libmad/layer12.c>
- Rockbox MPEG audio integration:
  <https://github.com/Rockbox/rockbox/blob/master/lib/rbcodec/codecs/mpa.c>
- Public iPod Photo 5.1.2.1 firmware research:
  <https://github.com/giek2000/ipod-classic-firmware-research/blob/main/specs/iPod_4th_Gen_Photo_5_1_2_1.md>

## RetailOS evidence boundary

The local `iPod_5.1.2.1.bin` analysis supports separate photo copy, artwork
load, render, display, and LCD-update tasks; object-held LCD state; queued
resources; and preformatted/pipelined content. It does **not** prove LCD DMA,
framebuffer swapping, unchecked FIFO writes, task-to-core assignment, or a
Rockbox-reusable external-buffer API. Firmware findings may generate bounded
experiments, but only device measurements promote an IPVF mechanism.
