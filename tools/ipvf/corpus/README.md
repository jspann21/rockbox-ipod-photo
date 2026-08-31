# Deterministic IPVF corpus

`tools/ipvf/generate_corpus.py` creates reproducible P0.3 source material for
host and device qualification. It uses only the Python standard library for
content generation. The preferred output is lossless FFV1 video with PCM audio
in the deterministic NUT container, which the existing `tools/ipvf/encode.py`
FFmpeg path can consume. Matroska remains available with `--format mkv`, but
its generated segment UID is not suitable for byte-identical corpus reruns.

Run from WSL/Linux:

```sh
python3 tools/ipvf/generate_corpus.py \
  --out /tmp/ipvf-p0.3-corpus \
  --profile standard \
  --seed 20260831 \
  --format nut
```

For a small smoke corpus, use `--profile quick --duration 1`. To retain the
intermediate RGB24 and WAV inputs for independent inspection, use
`--format both`. `--format raw` emits separate raw video and WAV files; those
are useful to an inspector but are not themselves a single encode.py input.

The generator refuses to write into a non-empty directory unless
`--overwrite` is supplied. Its default output is `/tmp/ipvf-p0.3-corpus`, and
the nested `.gitignore` also protects corpus artifacts if an in-repository
output directory is selected.

## Manifest contract

`manifest.json` is a top-level object with a `clips` list. All paths in a clip
are POSIX paths relative to the manifest directory, via the top-level
`source_root: "."` field. The normal lab-runner input is `clip["source"]`.

Each clip contains:

| Field | Meaning |
| --- | --- |
| `clip_id` | Stable profile identifier. |
| `source` | Relative FFV1/NUT source path (or Matroska/raw when explicitly selected). |
| `sha256` | SHA-256 of the primary `source` file. |
| `hashes` | Source, raw RGB24 frame-stream, PCM payload, and WAV SHA-256 values. |
| `dimensions` | Source width/height, generated `rgb24` format, and actual source stream format (`bgr0` for FFV1; `rgb24` for raw). |
| `fps` | Integer nominal rate; safe to pass directly to `encode.py`. |
| `source_fps` | Exact rational `{num, den, value}`, including 24000/1001 in `full`. |
| `duration_seconds` | Actual source video duration after integer frame rounding. |
| `frame_count` | Exact source video frame count. |
| `video` | Video duration, frame count, raw byte count, and scan type. |
| `audio` | Presence, pattern, channels, 44.1 kHz PCM properties, sample count, duration, and shorter/equal/longer relation. |
| `generator_parameters` | Pattern, motion class, per-clip derived seed, geometry/rate, requested duration, and pattern parameters. |

The top-level `generator` records the corpus seed, profile, output format,
generator version/hash, Python version, platform, and FFmpeg version. No wall
clock or temporary path is included, so rerunning with the same seed/profile/
format/toolchain produces the same generated content and source hashes.

The quick profile covers static, local object motion, global pan and scroll,
cuts/fades, grain and full noise, odd/even boundary changes, and shorter/longer
audio. The standard and full profiles add test-card/still/slideshow material,
two disjoint objects, subtitle/sprite changes, camera shake, alternating
frames, silence/mono/stereo-ID/impulse/clipping audio, vertical and letterboxed
source shapes, and several integer/rational source rates.
