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

Installed package: `dist/package-ipvf-qualification-v2-20260831`.

- Device directory: `Videos/IPVF Qualification 2`.
- Device log: `.rockbox/ipvf-qualification-v2.tsv`.
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
routine. V2 telemetry separates LZ4, temporal reconstruct, and copy timing.

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

Raw evidence: `dist/ipvf-qualification-results-v2-20260831`.

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

| Corpus/rate | V2 -> V3 gaps | V2 -> V3 temporal ms/XOR | V2 -> V3 decode ms/frame |
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
