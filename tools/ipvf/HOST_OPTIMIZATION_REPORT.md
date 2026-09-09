# IPVF host conversion: speed, pixel quality, and file-size experiments

Date: 2026-09-08, America/New_York. Baseline: `a0e9e0c700a51d685c704fa59a28dc859e6ee338`.
The recorded benchmark start is 2026-09-09 03:44 UTC (September 8 locally).

## Summary

This change accelerates the host creator without changing the IPVF bitstream or
firmware. With existing pixel conversion and optional NumPy available, median
complete CLI time fell 30.6–41.7% on the five changing-image fixtures. A static
fixture improved only 3.3%, which is too small to distinguish confidently from
host timing noise. The Python-only variant also had a slower static median;
not every run or configuration is faster. All default and Python-only output
files matched the baseline SHA-256 exactly on the six-fixture benchmark.

Two independent, opt-in controls address quality and size. `--quantizer native`
packs scaled BGR0 directly into RGB565BE, avoiding the tested FFmpeg build's
additional low-bit RGB conversion error. `--size-search` compares an additional
full compressed key against deltas, but accepts it only when it saves a whole
sector. Neither is enabled by default. The native quantizer retains 5/6/5 color
precision but changes pixels relative to the existing FFmpeg path; size search
is lossless relative to whichever quantizer is selected.

This is **host-only evidence, not new iPod hardware qualification**. No firmware
build, simulator run, physical A1099 playback, battery measurement, natural-film
corpus, or complete repository qualification suite was performed.

## What the implementation already does well

The current [encoder](encode.py) and the constants/compile guards in
[`ipodnative.c`](../../apps/plugins/ipodnative.c) implement a deliberately cheap
player format: 220x176 native RGB565SWAPPED pixels; raw/LZ4 keys and rectangles;
repeat, XOR, and translated-residual records; 512-byte record alignment; and
matching 44.1 kHz adaptive IMA audio in the same read. The player caps a record
at 192 sectors and indexes true keys with 16-byte entries. Keeping compression
work on the host is a good fit for this contract.

The baseline already uses official liblz4 HC12 when available, adaptive
silence/mono/stereo audio, rational cadence, bounded key intervals, metadata,
checksums, and mandatory independent post-encode validation. These are not new
features of this patch. The CLI's existing motion-at-up-to-30-fps policy and the
programmatic encoder's spatial default remain intact. The temporal >30-fps
restriction, two-pixel rectangle alignment, sector limit, 2-GiB guard, LZ4 block
semantics, audio coding, and on-device code are not relaxed or changed.

## Implemented changes

### Faster host work with the same default output

`rockbox_crc32` translates input bytes through a bit-reversal table, runs zlib's
native CRC, and explicitly reverses/complements its state. Rockbox uses the
non-reflected polynomial with no final XOR; a bare `zlib.crc32(data)` would be
incorrect. The original table algorithm remains available when Python has no
zlib. Tests cover the known answer `123456789 -> 0376e6e7`, non-default seeds,
empty data, incremental updates, and bounded file ranges against the independent
validator's nibble-at-a-time CRC.

Bounding-box discovery now compares whole rows in native code and only scans
LCD words beyond the current horizontal envelope. Tile discovery compares tile
slices, skips unchanged rows and already-marked tiles, and preserves component
merging, ordering, alignment, and the bounded component fallback. Fixed-width,
explicit-endian integer XOR replaces the Python per-byte XOR loop.

Record selection stops evaluating strictly unwinnable candidates once it has
reached `ceil((16 + audio_bytes) / 512)` sectors. Existing tie preferences are
preserved. The first compression call still validates compressor selection;
small records do not silently bypass an invalid mode or missing explicitly
requested liblz4.

`--motion-backend auto` lazily uses NumPy when available. It batches the same
sampled RGB565 **byte-SAD** calculations as the original estimator, not a new
motion metric. Signed subtraction prevents uint8 wraparound, 64-bit reductions
avoid accumulation overflow, and Python integers preserve normalized-score
cross-products. Sampling origins, overlap counts, coarse/refined search order,
and tie-breaking are unchanged. Batches are bounded to 65,536 sampled pixels.
`--motion-backend python` keeps the scalar path; `numpy` explicitly requires
NumPy when motion search is needed. NumPy is not a mandatory dependency.

### Optional direct RGB565 conversion

`--quantizer native` requests the existing scale/letterbox pipeline's BGR0
output, then takes each pixel's upper 5 red, 6 green, and 5 blue bits. Production
packing uses masked 32-bit integer lanes and strided byte compaction; it does
not require NumPy. It introduces no spatial averaging or dithering in the
packing stage. FFmpeg still performs source decoding and scaling.

The source oracle in [reference.py](reference.py) is a separate scalar
implementation using `struct.iter_unpack` and explicit RGB565 word packing.
[validate.py](validate.py) accepts the quantizer setting and still independently
checks every reconstructed frame, record chain, CRC, index, and audio decode.
No production quantization or fast CRC helper is imported into that oracle.
The creator's validation-report JSON records the selected quantizer. The
container does not need a new flag: it still stores ordinary RGB565 pixels.
Standalone source validation must be given the original quantizer setting.

An exhaustive test preserves all 65,536 RGB565 colors after expanding their
channels to 8 bits and repacking. On the native-resolution periodic fixture,
the complete source/encode/independent-validation pipeline also preserves every
source RGB565 byte. The default FFmpeg path did not on this host. This is an
observation about the tested pipeline and FFmpeg build, not a diagnosis of every
FFmpeg version or a claim that all upstream color/scaling operations are exact.

Direct truncation can expose gradient banding; an RGB PSNR score does not
establish subjective superiority. The BGR0 pipe is twice as large, and scalar
reference quantization adds validation work. Native mode is not advertised as
a universal speedup or a universal file-size reduction.

### Optional full-key size search

The baseline's initial raw-area decision can overlook a full frame whose LZ4
matches are destroyed by cropping to a rectangle. `--size-search` compares that
full-key candidate when useful. It does not recompress a key already tested,
including when a subsequent motion candidate won. It accepts only a strict
sector improvement, paying for the extra 16-byte index entry with at least
512 bytes of media savings (at least 496 net bytes per newly introduced key).
Scheduled key positions and dependency limits are retained.

The tradeoff matters on this player: a full key can require a full-screen LCD
transfer instead of a rectangle, and a denser index consumes more space/cache.
Saving disk bytes does not prove faster or lower-power playback. Hardware
requalification is required before making this policy the default.

## Repeated complete-conversion benchmark

[Raw timings and hashes](qualification/host-optimization-20260908.json) include
all three timed samples, source/output SHA-256, code fingerprints, record
counts, maximum record sectors, and decoded-audio CRCs.

Environment: CPython 3.13.5; Linux x86-64/glibc 2.41; Xeon Platinum 8573C;
5 exposed logical CPUs; NumPy 2.3.5; liblz4 1.10.0; FFmpeg 7.1.5-0+deb13u1.
These are measurements on this host, not recommended or minimum versions.
Each cell is the median of three fresh complete CLI processes after one
unmeasured warm-up per variant. Variant order rotates. Timing includes imports,
FFmpeg conversion, audio encoding, file/index/CRC finalization, and mandatory
independent source reconstruction plus audio decoding. Source generation is
outside the timer. No validation work was disabled.

| Synthetic fixture | Frames | Baseline seconds | Python backend seconds | Auto backend seconds | Auto time reduction | File bytes, unchanged |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Moving testsrc2 + stereo | 48 | 4.599 | 4.072 | 3.148 | 31.6% | 584,208 |
| Static image + dual mono | 48 | 1.728 | 2.005 | 1.671 | 3.3% | 77,840 |
| Sparse moving square, silent | 48 | 2.166 | 1.197 | 1.263 | 41.7% | 25,104 |
| Translated texture, silent | 48 | 3.188 | 2.513 | 2.211 | 30.6% | 165,392 |
| Seeded noise, silent | 24 | 3.141 | 2.069 | 1.891 | 39.8% | 1,868,672 |
| Periodic crop stress, silent | 24 | 2.560 | 2.063 | 1.691 | 33.9% | 90,128 |

These six fixtures contain 240 frames / 10 seconds of synthetic source content
at 24 fps. The moving testsrc2 fixture is downscaled from 440x352; the others
start at native resolution. The generator includes repeat, sparse-delta,
translation, incompressible, mono, stereo, and silence workloads.

All **96 full CLI invocations** (6 fixtures x 4 variants x 4 rounds) passed.
Both optimized default and Python-backend files matched the baseline byte for
byte. Size search produced no savings with the default FFmpeg quantizer on
these six fixtures; its targeted benefit appears in the next experiment.

There is visible run-to-run noise: for example, moving baseline samples span
4.283–5.902 seconds, and the Python static median is worse than baseline despite
the auto and Python paths avoiding NumPy work for repeats. Three samples do not
provide a strong confidence interval. Small differences and cross-suite timing
comparisons should not be overinterpreted. Longer natural clips on the actual
conversion machine are needed before projecting movie-length throughput.

## Quality and size experiment

[Raw quality metrics and timings](qualification/quantizer-20260908.json) use
three synthetic sources and the same one-warm-up/three-timed-round protocol.
All **36 full CLI invocations** passed independent source and audio validation.
The audio CRC is unchanged across quantizer and size-search choices.

RGB PSNR compares decoded RGB565 channels, expanded to 8 bits by bit replication,
against a separately requested scaled BGR0 reference using the same cadence,
geometry, Lanczos, and padding settings. It isolates error relative to this
explicit output reference, not the original high-resolution movie or the
physical LCD. The RGB gradient is generated at 8-bit channel precision, not
pre-quantized to RGB565. The periodic fixture additionally has an original raw
RGB565-byte oracle.

| Fixture | FFmpeg RGB PSNR | Native RGB PSNR | Default bytes | Native bytes | Native + size-search bytes |
| --- | ---: | ---: | ---: | ---: | ---: |
| Moving testsrc2 + stereo | 30.91 dB | 43.28 dB | 584,208 | 422,928 | 422,928 |
| Native periodic crop stress | 14.53 dB | Exact reference | 90,128 | 52,752 | 40,656 |
| 8-bit RGB gradient | 36.46 dB | 38.32 dB | 61,456 | 21,008 | 21,008 |

On the moving fixture, native mode reduces total file size by **27.6%** and
maximum channel error from 192 to 7 relative to the BGR0 reference. This is a
pixel-conversion change, not lossless compression of the baseline pixels.
On the periodic source, native mode has zero mismatched source bytes; the
FFmpeg path changes 1,814,256 of 1,858,560 source bytes across 24 frames.

Holding native pixels fixed, size search reduces the periodic fixture from
52,752 to 40,656 bytes: **22.9% lossless total-file savings**, including the index.
The number of true keys rises from 1 to 13. This intentionally targeted LZ4
stress fixture is not evidence for a 22.9% reduction on typical films. No
additional size-search savings occurred on the other two quality fixtures.

| Fixture | Optimized FFmpeg seconds | Native seconds | Native + size-search seconds |
| --- | ---: | ---: | ---: |
| Moving testsrc2 + stereo | 3.950 | 4.554 | 4.802 |
| Periodic crop stress | 1.823 | 1.911 | 1.804 |
| 8-bit RGB gradient | 1.287 | 1.501 | 1.403 |

These separately measured medians explicitly show that better measured RGB
fidelity/smaller files need not mean faster total conversion. Native packing,
larger pipes, changed compression work, and the additional scalar source oracle
all affect end-to-end cost; no isolated attribution is claimed from these totals.

## Regression coverage and reproduction

The unchanged `test_encode.py` (22 tests) and new
`test_host_optimizations.py` (14 tests) passed together: **36 tests, 3.920 seconds**
on the recorded host. New checks cover CRC compatibility/fallback, geometry and
edge alignment, multi-rectangle reconstruction and component caps, XOR length,
sector lower bounds, compressor errors, NumPy/scalar scores and search ties,
missing NumPy, redundant compression avoidance, full-key index accounting,
independent native quantization, partial-frame rejection, and all RGB565 colors.
These are the two suites actually run, not a claim about `test_lab.py` or every
other repository test.

An additional local differential experiment compared 108 geometry cases and
108 complete video-record selections/payloads against the pinned baseline;
all matched with optional quality/size changes disabled. The committed unit
and benchmark scripts provide the primary reproducible regression checks.

From the repository root, use a fresh scratch directory for generated media:

```sh
python3 tools/ipvf/test_encode.py
python3 tools/ipvf/test_host_optimizations.py

# Optional for production motion acceleration; required for quality metrics.
python3 -m pip install numpy

baseline_dir=$(mktemp -d)
for name in encode.py reference.py validate.py; do
  git show a0e9e0c700a51d685c704fa59a28dc859e6ee338:tools/ipvf/$name > "$baseline_dir/$name"
done
python3 tools/ipvf/benchmark_host.py \
  --baseline-dir "$baseline_dir" --work-dir /tmp/ipvf-host-bench --repeats 3
python3 tools/ipvf/benchmark_quantizer.py \
  --work-dir /tmp/ipvf-quality-bench --repeats 3
```

FFmpeg/ffprobe and the existing compressor dependencies still apply. The
benchmarks retain generated media, outputs, logs, and validation reports in the
work directories. Exact compressed bytes can vary with dependency versions;
compare baseline/candidate under the same environment. The committed result
JSON is compacted only for storage; its samples and metrics are unchanged.

Normal conversion takes the compatible fast path automatically:

```sh
python3 tools/ipvf/encode.py input.mp4 output.ipvf

# Opt into the quality experiment and lossless representation search.
python3 tools/ipvf/encode.py input.mp4 native.ipvf \
  --quantizer native --size-search

# Scalar motion fallback / troubleshooting.
python3 tools/ipvf/encode.py input.mp4 scalar.ipvf --motion-backend python

# Independent source validation must use the same quantizer.
python3 tools/ipvf/validate.py native.ipvf --source input.mp4 --quantizer native
```

## Highest-value next experiments (not implemented here)

First, profile complete conversions on longer natural content. Independent
validation still uses a Python CRC and scalar reference pixel operations;
accelerating it with a separately implemented native oracle may yield more
end-to-end benefit than another encoder-only micro-optimization. Keep the small
scalar implementation for differential tests rather than weakening validation.

Second, prototype a bounded, ordered worker pool for independent video candidate
compression. Preserve frame-pair inputs, audio-derived sector costs, record
order, and the serial IMA state chain. Measure inter-process copying, peak memory,
startup cost, and actual multicore scaling before exposing worker defaults.
A naive parallel loop over the current whole-frame encoder would change audio
state/order and could consume excessive memory.

Third, evaluate nearest-level or temporally stable ordered dithering, mild
source denoising, and content-adaptive rectangle/key policies on real cartoons,
live action, grain, text, fades, and gradients. Measure pixel error, subjective
banding, sector savings, and on-device decode/LCD cost together. Do not silently
reduce RGB565 precision or source cadence to claim a lossless size improvement.

Finally, re-run the repository's full qualification workflow and instrumented
A1099 playback before promoting native quantization or full-key search. Record
late frames, audio gaps/rebuffers, maximum decode/render latency, storage reads,
seek behavior, index footprint, and battery impact. A new codec, audio format,
or changed sector layout would require a separate player/format proposal, not
a host-only optimization patch.
