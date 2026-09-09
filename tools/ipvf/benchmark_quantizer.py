#!/usr/bin/env python3
"""Evaluate optional direct RGB565 quantization against scaled BGR0 pixels.

Requires NumPy for measurement, not for the production native quantizer.
PSNR here is RGB error against an explicit 8-bit scaled BGR0 reference, not a
perceptual score or a claim about the physical iPod LCD. Native RGB565 sources
also provide an exact round-trip test, independent of that reference choice.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import statistics
import subprocess
import sys
import time
from pathlib import Path

import numpy as np

try:
    from . import benchmark_host, reference
except ImportError:
    import benchmark_host
    import reference


HERE = Path(__file__).resolve().parent


def decoded_rgb(frames: list[bytes]):
    words = np.frombuffer(b"".join(frames), dtype=">u2").astype(np.int32)
    red, green, blue = words >> 11, (words >> 5) & 63, words & 31
    return np.stack(((red << 3) | (red >> 2),
                     (green << 2) | (green >> 4),
                     (blue << 3) | (blue >> 2)), axis=-1)


def quality(source: Path, ffmpeg: str) -> dict:
    filters = ("fps=24/1,scale=220:176:force_original_aspect_ratio=decrease:"
               "flags=lanczos,pad=220:176:(ow-iw)/2:(oh-ih)/2:black")
    bgr0 = subprocess.check_output([
        ffmpeg, "-nostdin", "-v", "error", "-i", str(source), "-an", "-vf", filters,
        "-pix_fmt", "bgr0", "-f", "rawvideo", "-"])
    target = np.frombuffer(bgr0, dtype=np.uint8).reshape(-1, 4)[:, [2, 1, 0]]
    result = {}
    for quantizer in ("ffmpeg", "native"):
        frames = list(reference.ffmpeg_frames(source, 24, ffmpeg, quantizer=quantizer))
        error = decoded_rgb(frames) - target.astype(np.int32)
        mse = float(np.mean(error.astype(np.float64) ** 2))
        result[quantizer] = {
            "rgb_mse": mse,
            "rgb_psnr_db": 10 * math.log10(255 ** 2 / mse) if mse else None,
            "exact_rgb_reference": mse == 0,
            "max_channel_error": int(np.max(np.abs(error))),
            "pixels_different_from_rgb_reference": int(np.count_nonzero(np.any(error, axis=1))),
            "rgb565_sha256": hashlib.sha256(b"".join(frames)).hexdigest(),
        }
        original = source.with_suffix(".rgb565")
        if original.exists():
            expected = original.read_bytes()
            actual = b"".join(frames)
            if len(expected) != len(actual):
                raise RuntimeError("native round-trip fixture length mismatch")
            result[quantizer]["native_source_byte_mismatches"] = sum(
                a != b for a, b in zip(expected, actual))
    return result


def make_gradient(root: Path, ffmpeg: str) -> Path:
    raw = root / "rgb-gradient.bgr0"
    frame = bytes(channel for y in range(176) for x in range(220)
                  for channel in (128, y * 255 // 175, x * 255 // 219, 255))
    raw.write_bytes(frame * 24)
    source = root / "rgb-gradient.nut"
    benchmark_host.checked([
        ffmpeg, "-nostdin", "-v", "error", "-y", "-f", "rawvideo",
        "-pixel_format", "bgr0", "-video_size", "220x176", "-framerate", "24",
        "-i", str(raw), "-c:v", "copy", "-f", "nut", str(source)])
    return source


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--work-dir", type=Path, required=True)
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("--ffmpeg", default="ffmpeg")
    parser.add_argument("--ffprobe", default="ffprobe")
    args = parser.parse_args()
    if args.repeats < 1:
        parser.error("--repeats must be positive")
    root = args.work_dir.resolve()
    root.mkdir(parents=True, exist_ok=True)
    generated = benchmark_host.make_sources(root, args.ffmpeg)
    sources = {name: generated[name]
               for name in ("testsrc2-stereo", "periodic-crop-stress")}
    sources["rgb-gradient"] = make_gradient(root, args.ffmpeg)
    result = {
        "scope": "full CLI with independent source/audio validation; RGB errors against scaled BGR0",
        "warmup_runs_per_variant_per_case": 1,
        "measured_runs_per_variant_per_case": args.repeats,
        "numpy": np.__version__,
        "python": sys.version.split()[0],
        "ffmpeg": subprocess.check_output([args.ffmpeg, "-version"], text=True).splitlines()[0],
        "candidate_sha256": {name: benchmark_host.digest(HERE / name)
                             for name in ("encode.py", "reference.py", "validate.py")},
        "cases": {},
    }
    variants = {"ffmpeg": [], "native": ["--quantizer", "native"],
                "native-size-search": ["--quantizer", "native", "--size-search"]}
    for name, source in sources.items():
        case = {"source_sha256": benchmark_host.digest(source),
                "quality": quality(source, args.ffmpeg), "variants": {}}
        result["cases"][name] = case
        order = list(variants)
        for round_index in range(args.repeats + 1):
            rotated = order[round_index % len(order):] + order[:round_index % len(order)]
            for variant in rotated:
                output = root / f"{name}-{variant}.ipvf"
                report_path = root / f"{name}-{variant}-validation.json"
                command = [sys.executable, str(HERE / "encode.py"), str(source), str(output),
                           "--ffmpeg", args.ffmpeg, "--ffprobe", args.ffprobe,
                           "--validation-report", str(report_path), *variants[variant]]
                started = time.perf_counter()
                completed = subprocess.run(command, text=True, stdout=subprocess.PIPE,
                                           stderr=subprocess.STDOUT, check=False)
                seconds = time.perf_counter() - started
                (root / f"{name}-{variant}.log").write_text(completed.stdout, encoding="utf-8")
                if completed.returncode:
                    raise RuntimeError(completed.stdout)
                report = json.loads(report_path.read_text(encoding="utf-8"))
                if report["status"] != "pass" or not report["source_verified"]:
                    raise RuntimeError("independent validation did not pass")
                sha256 = benchmark_host.digest(output)
                values = case["variants"].setdefault(variant, {
                    "seconds": [], "file_bytes": output.stat().st_size, "sha256": sha256,
                    "independent_validation": "pass", "frames": report["frames"],
                    "decoded_audio_crc": report["decoded_audio_crc"], "counts": report["counts"],
                    "index_count": report["index_count"],
                })
                if values["sha256"] != sha256:
                    raise RuntimeError("nondeterministic quantizer output")
                if round_index:
                    values["seconds"].append(seconds)
                    values["median_seconds"] = statistics.median(values["seconds"])
                print(f"{name}/{variant} round={round_index} {seconds:.3f}s "
                      f"{output.stat().st_size} bytes PASS", flush=True)
        if (case["variants"]["native-size-search"]["file_bytes"] >
                case["variants"]["native"]["file_bytes"]):
            raise RuntimeError("size search regressed file size")
        if len({v["decoded_audio_crc"] for v in case["variants"].values()}) != 1:
            raise RuntimeError("quantization changed audio")
        (root / "results.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(f"Results: {root / 'results.json'}")


if __name__ == "__main__":
    main()
