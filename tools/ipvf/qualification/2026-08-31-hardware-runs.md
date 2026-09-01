# IPVF qualification runs - 2026-08-31

Status: passes 1 through 5 collected and analyzed. The checked v5 decoder is
installed; qualification files/logs are archived locally and logging is off.

## Rollback and evidence

- Device firmware: `dddffd0151M-260831`, target `ipodcolor`.
- Firmware SHA-256:
  `8DC29B572F0EEEE69DFC9471FE6FAE6BEB2BF9EC35F15D6FFA3FB9B67E26F3D7`.
- Complete prior `.rockbox` backup:
  `dist/ipod-before-ipvf-xor-20260831/.rockbox`.
- Pass-1 raw evidence:
  `dist/ipvf-qualification-results-20260831`.
- Logging remains gated by `.rockbox/ipvf-qualification.enable`.

## Pass 1 - current versus initial temporal decoder

The 12 files under `Videos/IPVF Qualification` all completed with the expected
240/480 frames, zero parser/decoder errors, zero render failures, and the
expected final framebuffer CRC. The PP5020 snapshot also reported zero DMA
timeouts, PIO fallbacks, DMA IRQ anomalies, PCM underruns, and timeout
registration failures. Timeout high-water was 1 of 8.

The initial temporal decoder failed the real-time promotion gate:

| Corpus/rate | Size saving | Baseline -> XOR gaps | Baseline -> XOR video decode ms/frame | XOR peak decode |
| --- | ---: | ---: | ---: | ---: |
| high motion, 30 fps | 18.2% | 0 -> 47 | 13.73 -> 37.87 | 42.11 ms |
| high motion, 60 fps | 28.5% | 45 -> 140 | 13.72 -> 40.14 | 42.22 ms |
| music, 30 fps | 8.1% | 0 -> 24 | 11.43 -> 24.20 | 46.49 ms |
| music, 60 fps | 7.9% | 1 -> 52 | 5.73 -> 12.05 | 46.40 ms |
| local motion, 30 fps | 2.7% | 0 -> 0 | 0.96 -> 1.87 | 34.46 ms |
| local motion, 60 fps | 5.8% | 0 -> 10 | 0.56 -> 1.56 | 34.75 ms |

Read and LCD/render timing did not explain the regression. Temporal
reconstruction was the added CPU cost. Temporal `auto` was not promoted and
remains explicit and experimental.

## Pass 2 - spatial-only candidate plus optimized temporal decoder

Installed package: `dist/package-ipvf-qualification-pass2-20260831`.

- Device directory: `Videos/IPVF Qualification 2`.
- Device log: `.rockbox/ipvf-qualification-pass2.tsv`.
- Decoder revision: `xor-fastcrc-1`.
- `ipodnative.rock` SHA-256:
  `6D65E7191A2DDD11B5A6B87A11517CDE74BF86DEB556B52330E7E75856384116`.
- Ten unit/contract tests pass in both direct and module invocation.
- All 12 packaged files pass strict record, padding, LZ4, IMA, temporal CRC,
  EOF, and final-frame validation.
- Every spatial-only file was compared frame-by-frame with fresh FFmpeg output.
- Package and device hashes match for the plugin and all 12 files.

The optimized decoder fuses temporal XOR and CRC into one cached pass and uses
a generated 256-entry CRC table instead of Rockbox's space-optimized two-nibble
routine. Pass-2 telemetry separates LZ4, temporal reconstruct, and copy timing.

Spatial-only host savings versus the pass-1 current files are:

| Corpus/rate | Current bytes | Spatial bytes | Saving |
| --- | ---: | ---: | ---: |
| high motion, 30 fps | 4,815,360 | 4,375,552 | 9.1% |
| high motion, 60 fps | 9,285,632 | 8,227,840 | 11.4% |
| music, 30 fps | 4,640,768 | 4,640,256 | 0.01% |
| music, 60 fps | 4,713,984 | 4,713,984 | 0.0% |
| local motion, 30 fps | 897,024 | 875,520 | 2.4% |
| local motion, 60 fps | 1,024,512 | 972,800 | 5.0% |

## Pass 2 - device results

Raw evidence: `dist/ipvf-qualification-results-pass2-20260831`.

All 12 files completed with correct frame counts and framebuffer CRCs, zero
decoder errors, and zero render failures. Spatial compression passed its
promotion gate: it reduced size for four of six workloads, never increased
size, and matched or improved decode/render time. It is now the encoder default.

| Corpus/rate | Spatial saving | Current -> spatial gaps | Current -> spatial decode ms/frame |
| --- | ---: | ---: | ---: |
| high motion, 30 fps | 9.1% | 0 -> 0 | 13.73 -> 11.63 |
| high motion, 60 fps | 11.4% | 45 -> 7 | 13.72 -> 11.21 |
| music, 30 fps | 0.01% | 0 -> 0 | 11.43 -> 11.44 |
| music, 60 fps | 0.0% | 1 -> 0 | 5.73 -> 5.74 |
| local motion, 30 fps | 2.4% | 0 -> 0 | 0.96 -> 0.93 |
| local motion, 60 fps | 5.0% | 0 -> 0 | 0.56 -> 0.51 |

The fused temporal decoder materially improved over pass 1, but temporal 60 fps
still failed the promotion gate. File 08 visibly stuttered and logged 55 gaps.
High-motion temporal 60 fps retained 140 gaps. Temporal XOR remains an explicit
experimental option.

| Corpus/rate | Pass-1 -> pass-2 gaps | Pass-1 -> pass-2 decode ms/frame |
| --- | ---: | ---: |
| high motion, 30 fps | 47 -> 0 | 37.87 -> 24.63 |
| high motion, 60 fps | 140 -> 140 | 40.14 -> 25.52 |
| music, 30 fps | 24 -> 1 | 24.20 -> 18.30 |
| music, 60 fps | 52 -> 55 | 12.05 -> 9.13 |
| local motion, 30 fps | 0 -> 0 | 1.87 -> 1.57 |
| local motion, 60 fps | 10 -> 1 | 1.56 -> 1.17 |

## Pass 3 - slicing-by-four temporal decoder

Installed package: `dist/package-ipvf-qualification-v3-20260831`.

- Device directory: `Videos/IPVF Qualification 3`.
- Device log: `.rockbox/ipvf-qualification-v3.tsv`.
- Decoder revision: `xor-slice4-2`.
- `ipodnative.rock` SHA-256:
  `600CF4817A54B4BC468CA9168658DD01577FF41411786D91F85B9526465432FE`.
- Temporal CRC now consumes four bytes per loop while preserving the exact
  per-frame integrity check.
- CRC tables live in the audio buffer, preserving plugin BSS headroom.
- Audio prebuffer is increased from one second to two seconds.
- Eleven unit/contract tests pass; all six files pass strict validation.
- Package and device hashes match for the plugin and all six files.

### Pass-3 device result

Raw evidence: `dist/ipvf-qualification-results-v3-20260831`.

All six files completed with correct frame counts and final framebuffer CRCs,
zero decoder errors, and zero render failures. The spatial control looked
correct. The intentionally silent high-motion files 1--3 looked correct; music
30 fps looked and sounded correct; music 60 fps visibly stuttered.

Slicing-by-four failed its performance gate. Temporal reconstruction increased
from about 13.8 to 15.7 ms per XOR record, and total decode time increased for
all five paired temporal workloads. The two-second prebuffer reduced
high-motion 60 fps gaps from 140 to 120 and music 60 fps gaps from 55 to 44,
but did not make either workload acceptable.

| Corpus/rate | Pass 2 -> 3 gaps | Pass 2 -> 3 temporal ms/XOR | Pass 2 -> 3 decode ms/frame |
| --- | ---: | ---: | ---: |
| high motion, 30 fps | 0 -> 4 | 13.78 -> 15.75 | 24.63 -> 26.38 |
| high motion, 60 fps | 140 -> 120 | 13.75 -> 15.72 | 25.52 -> 27.44 |
| music, 30 fps | 1 -> 0 | 13.78 -> 15.75 | 18.30 -> 19.08 |
| music, 60 fps | 55 -> 44 | 13.75 -> 15.72 | 9.13 -> 9.52 |
| local motion, 60 fps | 1 -> 4 | 13.79 -> 15.78 | 1.17 -> 1.22 |

The slicing-by-four implementation is retired. It is not a production
candidate and is not retained merely because some gap counts varied favorably.

## Pass 4 - temporal reconstruction lower bound

Installed package: `dist/package-ipvf-qualification-v4-20260831`.

- Device directory: `Videos/IPVF Qualification 4`.
- Device log: `.rockbox/ipvf-qualification-v4.tsv`.
- Decoder revision: `xor-nocrc-bound-3`.
- `ipodnative.rock` SHA-256:
  `613F8F3FA6E6B4073508EC537F066CA71B87885650F17FE173D2AE21B8AAA8CE`.
- Ten host unit/contract tests pass.
- All five files pass strict host validation, including every stored temporal
  CRC and final reconstructed frame.
- Package/device hashes match for the plugin and all five files.

This diagnostic decoder skips the expensive full-frame CRC during device
playback but leaves the stored CRC and strict host validation unchanged. It is
not a production integrity policy. It measures the best realistic temporal XOR
reconstruction cost so we can decide whether to design a cheaper on-device
checksum or abandon full-frame temporal XOR on this CPU.

### Pass-4 device result

Raw evidence: `dist/ipvf-qualification-results-v4-20260831`.

All five files completed with correct frame counts/final CRCs and zero decoder
or render errors. Music 30 fps looked and sounded good; music 60 fps visibly
stuttered. Plain aligned XOR costs a stable 6.13--6.14 ms per dependent frame.

| Corpus/rate | Decode ms/frame | Peak decode ms | Gaps | Result |
| --- | ---: | ---: | ---: | --- |
| high motion, 30 fps | 17.77 | 19.75 | 0 | correct |
| high motion, 60 fps | 17.99 | 19.50 | 95 | fails real time |
| music, 30 fps | 15.23 | 23.92 | 0 | correct audiovisual |
| music, 60 fps | 7.61 | 23.89 | 27 | visible stutter |
| local motion, 60 fps | 0.96 | 12.25 | 0 | correct |

Dense temporal 60 fps cannot fit the 16.67 ms frame budget even with no
integrity-check cost. Temporal mode is therefore capped at 30 fps; 60 fps uses
the hardware-proven spatial selector.

## Pass 5 - compressed-payload CRC at 30 fps

Installed package: `dist/package-ipvf-qualification-v5-20260831`.

- Device directory: `Videos/IPVF Qualification 5`.
- Device log: `.rockbox/ipvf-qualification-v5.tsv`.
- Decoder revision: `xor-payloadcrc-4`.
- `ipodnative.rock` SHA-256:
  `E2DF8A996DD51CEE4D2AB21317A59889C56A0AA5BA8D2FE93297C576C31A45A9`.
- Eleven host tests pass.
- The encoder and CLI reject temporal `auto` above 30 fps.
- Type 5 checks the compressed residual before LZ4 decoding, avoiding the
  expensive reconstructed-frame CRC pass while retaining storage/read
  corruption detection.
- All three files pass strict payload-CRC, LZ4, IMA, reconstruction, source
  frame, padding, chain, and EOF validation.
- Package/device hashes match for the plugin and all three files.

### Pass-5 device result

Raw evidence: `dist/ipvf-qualification-results-v5-20260831`.

All three files looked correct, the music file sounded correct, and every run
completed with the expected frame count/final CRC, zero decoder/render errors,
and zero audio gaps.

| Corpus, 30 fps | Payload check ms/XOR | XOR ms/XOR | Decode ms/frame | Peak ms | Gaps |
| --- | ---: | ---: | ---: | ---: | ---: |
| high motion | 4.58 | 6.23 | 21.89 | 24.90 | 0 |
| music | 5.96 | 6.22 | 17.63 | 32.62 | 0 |
| local motion | 0.34 | 6.22 | 1.42 | 12.49 | 0 |

The compressed-payload CRC design passes the short hardware gate at 30 fps.
It is promoted as the checked temporal implementation, while `spatial` remains
the friendly default pending long-duration drift/lifecycle qualification.
All qualification directories, TSVs, and the logging marker were removed from
the iPod and archived under `dist/ipod-removed-old-ipvf-tests-20260831`.

## Pass 6 - real-footage host-profile LCD A/B

- Production viewer size: 13,924 bytes.
- Logging and the qualification marker remained disabled.
- Five 15-second profiles used identical beginning/middle/end scenes from the
  `suds` real-footage sample: native-24, native-20, RGB454/24, RGB444/24, and
  RGB444/20.
- Installed contents matched the validated host outputs.

### Pass-6 device result

No immediate visual difference was apparent among the five profiles. All
played smoothly, with no stuttering or other audio/video issue. No objective
20 fps quality defect was established; a concern about how the numeric label
might sound is not technical test evidence. Native 30/60 footage is required
to evaluate genuinely higher-rate motion; duplicated frames from this source
cannot settle it.

Volume could not be adjusted in this pass. This isolated the missing viewer
input mapping from the already-correct audio decoder and mixer behavior.

## Pass 7 - combined playback and wheel-volume qualification

- The production player drains at most 16 queued button events per video frame.
- Clockwise/counter-clockwise wheel events aggregate into one clamped Rockbox
  volume update per frame; MENU stop and USB handling remain intact.
- The installed viewer was 14,112 bytes with qualification logging disabled.
- Ordinary track playback, IPVF playback, and wheel volume adjustment all
  worked in one combined device pass.
- No stutter, audio gaps, or other playback issue was observed.

The core volume control is accepted. A native-pipeline volume indicator and an
explicit minimum/maximum/rapid-wheel stress matrix remain future usability
qualification; neither blocks ordinary volume adjustment.

## Pass 8 - complete RGB444 lifecycle and gradient quality

- The complete 224.71-second RGB444/24 file passed strict validation directly
  from device storage before playback.
- Full playback, live volume changes, MENU stop, reopen, and brief replay all
  worked. No stutter or audio gap was observed.
- Dark-to-light transitions showed stepped contours rather than smooth fades.
  This is visible color banding and rejects RGB444 as the everyday default.
- No objective problem was observed with 20 fps in the earlier comparison; its
  use remains a valid size choice independent of the RGB444 banding result.
- Matched full native, RGB454, and RGB444 files are installed for a focused
  gradient comparison. RGB454 is 15.83% smaller than native and 10.34% larger
  than RGB444.

### Pass-8 focused gradient result

- Native RGB565 showed no visible banding.
- RGB454 showed noticeable banding, though less than RGB444.
- RGB444 showed very noticeable banding.

RGB454 and RGB444 are rejected as everyday defaults. This quality result does
not change the successful playback/lifecycle result or establish any problem
with 20 fps.

## Pass 9 - RGB555 gradient candidate

- Full RGB555/24 size: 140,560,896 bytes, 8.40% below native.
- The complete file passed strict frame/source, LZ4, IMA, chain, padding, and
  EOF validation both on the host and directly from device storage.
- Installed comparison order is native, RGB555, RGB454, RGB444.
- RGB555 showed noticeable banding. Its 8.40% saving was not considered worth
  the visible quality loss, so everyday remains RGB565.

## Pass 10 - full-color compact host qualification

- Compact now means 20 fps with full RGB565 color, separating cadence/storage
  choice from the rejected color-bit reductions.
- Complete size: 129,637,888 bytes, 15.52% below native RGB565/24.
- All 4,494 frames passed strict source reconstruction, LZ4, IMA, sector-chain,
  padding, and EOF validation.
- The prior short A1099 comparison showed no objective 20 fps playback or
  motion defect. A complete-file device run remains optional lifecycle evidence.

## Pass 11 - indexed navigation and pause

- The indexed IPVF file passed strict validation before installation.
- Play pause/resume, repeated ten-second backward/forward seeks, live volume,
  MENU exit and reopen, normal completion, picture, sound, and A/V sync all
  worked correctly on A1099 hardware.
- Indexed seeking and pause are accepted. Persistent resume is the next
  navigation feature; USB interruption remains in the broader lifecycle matrix.
- Production playback now exits silently after normal completion or MENU stop.
  Only a genuine playback failure presents a short error message.
- IPVF remains one unreleased canonical format. No version field, legacy parser,
  or compatibility path is retained.

### Rapid-seek follow-up

Rapid multi-click seeking exposed burst coalescing and reconstruction-time
input loss. The corrected player counts distinct press events, preserves
requests received during reconstruction, ignores release/repeat noise, and
stops only on a real MENU press. Extensive back-and-forth hardware input then
worked correctly. A later browser return was normal completion: one ten-second
jump from roughly 30 seconds in the 45-second clip leaves only about five
seconds. No special EOF clamp or diagnostic build is retained.

## Pass 12 - resume and metadata candidate

- Candidate scope: content-derived media identity, alternating CRC-protected
  resume slots, presentation-safe periodic/event checkpoints, atomic
  active/dismissed/complete state, resume/start-over prompt, and a
  Select-triggered metadata/details screen.
- Resume enters the existing indexed keyframe reconstruction and exact audio
  sample reset path; details pause the mixer and drain render ownership before
  drawing, then restore the exact reference frame.
- Host suite: 22 passing WSL tests, including corruption of an otherwise valid
  audio payload being rejected by the media-identity check.
- USB/power/reboot handling checkpoints the last confirmed presentation and
  closes media/audio/render ownership before Rockbox storage takeover.
- A1099 hardware qualification passed. Center Select opened details and returned
  to coherent playback; MENU/reopen resumed correctly; declining resume started
  from the beginning; natural completion suppressed the next resume prompt.
- The 45-second completion run crossed the 30-second periodic checkpoint with no
  reported stutter, desynchronization, wrong frame, or unexpected exit.
- Reboot, USB-takeover, and forced-interruption recovery remain lifecycle cases.

## Pass 13 - exact cadence and adaptive audio candidate

- Candidate uses the canonical 16-byte media-record header with explicit audio
  payload length and exact 24000/1001 frame timing.
- One 15-second file transitions through approximately five seconds each of
  zero-payload silence, exact dual-mono IMA, and deliberately audible stereo
  IMA: 119/120/120 records respectively.
- Strict source validation passes all 359 frames and reports final framebuffer
  CRC `cf0f82d3`; 17 focused host tests and the production A1099 build pass.
- Hardware qualification passed. The first generated stereo-control source was
  effectively inaudible at about -79.5 dB RMS and was replaced rather than
  treated as a decoder failure. The corrected file produced expected silence,
  centered mono, audible stereo, and clean transitions; the 45-second exact-
  cadence file also played and operated correctly.

## Pass 14 - whole-frame motion prediction

- The canonical translated-residual record stores signed `dx`/`dy`, a
  Rockbox CRC32 of the compressed residual, and one independent raw LZ4 block.
- The encoder compares complete sector-rounded costs and retains motion only
  when it beats the best spatial candidate. Forced true keys bound every
  dependency chain and remain the only index entries.
- The 45.09-second 24000/1001-fps real-footage candidate used 573 motion
  records among 1,081 frames. It measured 24,687,264 bytes versus 27,835,040
  bytes for the matched spatial control: 3,147,776 bytes or 11.3% smaller for
  the whole file, including identical adaptive audio behavior.
- Strict host validation reconstructed all 1,081 RGB565 frames exactly and
  verified the record chain, CRCs, padding, audio blocks, metadata, media
  identity, and keyframe index. Nineteen focused host tests and the production
  WSL A1099 build passed.
- Matched A1099 comparison found no visible or operational difference. The
  motion file played correctly with working volume, pause/resume, and repeated
  indexed seeking. Motion is accepted as the creator default through 30 fps;
  output above 30 fps continues to default to spatial records.

## Pass 15 - timed keys and 96-KiB render slots

- The creator replaces its frame-count CLI default with a positive
  `--key-seconds` duration. Five seconds maps through exact cadence to 119
  frames at 24000/1001, 150 at 30 fps, and 300 at 60 fps.
- The 45.09-second candidate contains indexed true keys at
  `0,119,238,357,476,595,714,833,952,1071`; maximum spacing is 119 frames.
  All 1,081 source frames reconstruct exactly with final framebuffer CRC
  `517baebe`.
- The three render slots use a conservative 96-KiB stride instead of 128 KiB,
  freeing 98,304 bytes total. The parser's maximum decoded record plus header
  is 81,552 bytes, enforced by a compile-time assertion.
- Twenty focused host tests, strict source validation, and the production WSL
  A1099 build pass. On hardware, the spatial control and timed-key/motion file
  showed correct picture and sound with working volume, pause/resume, Details,
  rapid forward/back seeking, MENU/reopen, and persistent resume. No stutter,
  visual corruption, audio issue, or unexpected exit was observed.

## Pass 16 - silent sources and native 30/60 motion

- The creator probes for an audio stream before invoking FFmpeg audio decode.
  A source with no audio now produces exact zero-payload silence records rather
  than failing the required audio map; decoded timing remains 44.1-kHz stereo.
- One eight-second native-60 high-motion source was encoded at matched native
  cadence outputs. The 30-fps file contains 240 frames, 216 motion records, no
  stored audio, and 3,587,632 bytes. The 60-fps file contains 480 spatial
  frames, no stored audio, and 7,865,536 bytes.
- Strict validation reconstructed every output frame from the source. Final
  framebuffer CRCs are `5b2183e9` at 30 fps and `d9a00934` at 60 fps; every
  audio mode is exact silence.
- Both files passed A1099 playback and control checks without stutter,
  corruption, noise, or unexpected exit. The 60-fps presentation felt
  faster/more immediate than 30 fps. Both files retain the same exact
  eight-second duration, so this is a motion-perception result rather than a
  playback-clock speed difference.

## Pass 17 - 24-to-60 host interpolation rejection

- Three matched eight-second files used identical audio and final framebuffer
  content: source cadence (192 frames, 4,269,600 bytes), ordinary duplicated
  60 fps (480 frames, 4,713,504 bytes), and motion-interpolated 60 fps (480
  frames, 10,768,928 bytes).
- Strict host validation reconstructed all three against their exact FFmpeg
  filter outputs. The interpolator initially emitted only 476 frames; bounded
  cloned-tail padding plus exact trimming corrected it to 480 before hardware
  testing.
- Source cadence played cleanly. Duplicated 60 fps showed slight stuttering.
  Interpolated 60 fps showed heavy stuttering throughout and cost 2.28 times
  as much storage as duplicated 60.
- Motion interpolation is rejected and removed from the uncommitted creator
  and validator changes. Native cadence remains the preferred default; 60 fps
  is most appropriate for genuinely native high-rate sources with measured
  device margin.
