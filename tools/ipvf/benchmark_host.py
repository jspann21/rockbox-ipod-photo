#!/usr/bin/env python3
"""Reproducible full-CLI benchmarks; no external media or skipped validation.

The baseline directory must contain encode.py, reference.py and validate.py
from the revision being compared. Sources and outputs stay in --work-dir.
Every measured run starts a fresh Python process and performs the creator's
normal independent FFmpeg source reconstruction and audio decode validation.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import platform
import random
import statistics
import struct
import subprocess
import sys
import time
from datetime import datetime, timezone
from pathlib import Path

try:
    from . import encode as ipvf
except ImportError:
    import encode as ipvf


HERE = Path(__file__).resolve().parent


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def checked(command: list[str]) -> None:
    subprocess.run(command, check=True, stdout=subprocess.DEVNULL)


def make_sources(root: Path, ffmpeg: str) -> dict[str, Path]:
    sources = {}
    moving = root / "testsrc2-stereo.nut"
    checked([ffmpeg, "-nostdin", "-v", "error", "-y", "-f", "lavfi", "-i",
             "testsrc2=size=440x352:rate=24:duration=2", "-f", "lavfi", "-i",
             "aevalsrc=0.15*sin(2*PI*440*t)|0.15*sin(2*PI*660*t):s=44100:d=2",
             "-c:v", "ffv1", "-c:a", "pcm_s16le", "-shortest", str(moving)])
    sources["testsrc2-stereo"] = moving
    rng = random.Random(20260908)
    base = rng.randbytes(ipvf.FRAME_BYTES)
    motif = rng.randbytes(1024)
    clean = (motif * (ipvf.FRAME_BYTES // 1024 + 1))[:ipvf.FRAME_BYTES]
    stripe_motif = rng.randbytes(1024)
    stripe = (stripe_motif * 52)[:ipvf.H * 300]
    covered = bytearray(clean)
    for y in range(ipvf.H):
        offset = (y * ipvf.W + 20) * 2
        covered[offset:offset + 300] = stripe[y * 300:(y + 1) * 300]
    covered = bytes(covered)
    gradient = b"".join(struct.pack(">H", ((x * 31 // (ipvf.W - 1)) << 11) |
                                   ((y * 63 // (ipvf.H - 1)) << 5) | 12)
                        for y in range(ipvf.H) for x in range(ipvf.W))
    sparse = []
    for n in range(48):
        frame = bytearray(ipvf.FRAME_BYTES)
        for y in range(80, 84):
            offset = (y * ipvf.W + 2 * n) * 2
            frame[offset:offset + 8] = b"\xf8\x00" * 4
        sparse.append(bytes(frame))
    cases = {
        "static-mono": [gradient] * 48,
        "sparse-silent": sparse,
        "translation-silent": [ipvf.translate_frame(base, 2 * (n % 9),
                                                     2 * (n % 5)) for n in range(48)],
        "noise-silent": [rng.randbytes(ipvf.FRAME_BYTES) for _ in range(24)],
        "periodic-crop-stress": [covered, clean] * 12,
    }
    for name, frames in cases.items():
        raw = root / f"{name}.rgb565"
        with raw.open("wb") as stream:
            for frame in frames:
                stream.write(frame)
        source = root / f"{name}.nut"
        command = [ffmpeg, "-nostdin", "-v", "error", "-y", "-f", "rawvideo",
                   "-pixel_format", "rgb565be", "-video_size", "220x176",
                   "-framerate", "24", "-i", str(raw)]
        if name == "static-mono":
            pcm = root / "mono.pcm"
            with pcm.open("wb") as stream:
                for n in range(ipvf.AUDIO_SAMPLE_RATE * 2):
                    value = round(6000 * math.sin(2 * math.pi * 440 * n /
                                                  ipvf.AUDIO_SAMPLE_RATE))
                    stream.write(struct.pack("<hh", value, value))
            command += ["-f", "s16le", "-ar", "44100", "-ac", "2", "-i", str(pcm),
                        "-c:a", "pcm_s16le", "-shortest"]
        checked(command + ["-c:v", "copy", "-f", "nut", str(source)])
        sources[name] = source
    return sources


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline-dir", type=Path, required=True)
    parser.add_argument("--work-dir", type=Path, required=True)
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("--ffmpeg", default="ffmpeg")
    parser.add_argument("--ffprobe", default="ffprobe")
    parser.add_argument("--case", action="append", help="limit to named fixtures")
    args = parser.parse_args()
    if args.repeats < 1:
        parser.error("--repeats must be positive")
    baseline = args.baseline_dir.resolve()
    for name in ("encode.py", "reference.py", "validate.py"):
        if not (baseline / name).is_file():
            parser.error(f"missing baseline file: {baseline / name}")
    root = args.work_dir.resolve()
    root.mkdir(parents=True, exist_ok=True)
    sources = make_sources(root, args.ffmpeg)
    if args.case:
        unknown = set(args.case) - set(sources)
        if unknown:
            parser.error(f"unknown case(s): {sorted(unknown)}")
        sources = {name: source for name, source in sources.items() if name in args.case}
    variants = {
        "baseline": (baseline / "encode.py", []),
        "python": (HERE / "encode.py", ["--motion-backend", "python"]),
        "auto": (HERE / "encode.py", []),
        "size-search": (HERE / "encode.py", ["--size-search"]),
    }
    try:
        import numpy
        numpy_version = numpy.__version__
    except ImportError:
        numpy_version = None
    compressor = ipvf.official_lz4()
    ffmpeg_version = subprocess.check_output(
        [args.ffmpeg, "-version"], text=True).splitlines()[0]
    cpu = platform.processor()
    cpuinfo = Path("/proc/cpuinfo")
    if cpuinfo.exists():
        cpu = next((line.split(":", 1)[1].strip() for line in cpuinfo.read_text().splitlines()
                    if line.startswith("model name")), cpu)
    result = {
        "created_utc": datetime.now(timezone.utc).isoformat(),
        "scope": "fresh full CLI process including independent source/audio validation",
        "warmup_runs_per_variant_per_case": 1,
        "measured_runs_per_variant_per_case": args.repeats,
        "environment": {"python": platform.python_version(), "platform": platform.platform(),
                        "cpu": cpu, "logical_cpus": os.cpu_count(), "numpy": numpy_version,
                        "liblz4": compressor.version if compressor else None,
                        "ffmpeg": ffmpeg_version},
        "baseline_sha256": {name: digest(baseline / name)
                            for name in ("encode.py", "reference.py", "validate.py")},
        "candidate_sha256": {name: digest(HERE / name)
                             for name in ("encode.py", "reference.py", "validate.py")},
        "cases": {},
    }
    result_path = root / "results.json"
    for name, source in sources.items():
        case = {"source_sha256": digest(source), "variants": {}}
        result["cases"][name] = case
        order = list(variants)
        # One discarded warm-up followed by rotated-order timed rounds.
        for round_index in range(args.repeats + 1):
            round_order = order[round_index % len(order):] + order[:round_index % len(order)]
            for variant in round_order:
                encoder, extra = variants[variant]
                output = root / f"{name}-{variant}.ipvf"
                report_path = root / f"{name}-{variant}-validation.json"
                command = [sys.executable, str(encoder), str(source), str(output),
                           "--ffmpeg", args.ffmpeg, "--ffprobe", args.ffprobe,
                           "--validation-report", str(report_path), *extra]
                started = time.perf_counter()
                completed = subprocess.run(command, text=True, stdout=subprocess.PIPE,
                                           stderr=subprocess.STDOUT, check=False)
                seconds = time.perf_counter() - started
                (root / f"{name}-{variant}.log").write_text(completed.stdout, encoding="utf-8")
                if completed.returncode:
                    raise RuntimeError(f"{name}/{variant} failed:\n{completed.stdout}")
                report = json.loads(report_path.read_text(encoding="utf-8"))
                if report["status"] != "pass" or not report["source_verified"]:
                    raise RuntimeError(f"independent validation failed: {name}/{variant}")
                sha256 = digest(output)
                values = case["variants"].setdefault(variant, {
                    "seconds": [], "sha256": sha256, "file_bytes": output.stat().st_size,
                    "frames": report["frames"], "decoded_audio_crc": report["decoded_audio_crc"],
                    "counts": report["counts"], "index_count": report["index_count"],
                    "max_record_sectors": report["max_record_sectors"],
                    "independent_validation": "pass",
                })
                if values["sha256"] != sha256:
                    raise RuntimeError(f"nondeterministic output: {name}/{variant}")
                if round_index:
                    values["seconds"].append(seconds)
                    values["median_seconds"] = statistics.median(values["seconds"])
                print(f"{name}/{variant} round={round_index} {seconds:.3f}s "
                      f"{output.stat().st_size} bytes PASS", flush=True)
        baseline_values = case["variants"]["baseline"]
        for variant, values in case["variants"].items():
            if variant in ("python", "auto") and values["sha256"] != baseline_values["sha256"]:
                raise RuntimeError(f"default output differs from baseline: {name}/{variant}")
            if values["decoded_audio_crc"] != baseline_values["decoded_audio_crc"]:
                raise RuntimeError(f"audio changed: {name}/{variant}")
            if values["file_bytes"] > baseline_values["file_bytes"]:
                raise RuntimeError(f"file size regressed: {name}/{variant}")
        result_path.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(f"Results: {result_path}")


if __name__ == "__main__":
    main()
