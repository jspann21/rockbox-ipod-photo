#!/usr/bin/env python3
"""Measure bounded whole-frame translation residuals without changing IPVF."""
from __future__ import annotations

import argparse
from fractions import Fraction
from pathlib import Path

try:
    from . import encode as ipvf
except ImportError:
    import encode as ipvf


def measure(source: Path, ffmpeg: str, ffprobe: str, max_shift: int,
            keyint: int, lz4_mode: str) -> dict[str, int | str | float]:
    rate = min(Fraction(30), ipvf.probe_source_fps(source, ffprobe))
    previous = None
    baseline_sectors = 0
    adaptive_sectors = 0
    motion_records = 0
    frames = 0
    for current in ipvf.ffmpeg_frames(source, rate, ffmpeg):
        audio_frames = (ipvf.audio_boundary(frames + 1, rate) -
                        ipvf.audio_boundary(frames, rate))
        audio_size = 8 + audio_frames - 1
        force_key = previous is None or (keyint > 0 and frames % keyint == 0)
        baseline = ipvf.choose_video_record(
            previous, current, audio_size, force_key,
            "spatial", 8, lz4_mode,
        )
        baseline_cost = ipvf.record_sectors(len(baseline[2]), audio_size)
        selected_cost = baseline_cost
        if previous is not None and not force_key:
            dx, dy = ipvf.estimate_translation(
                previous, current, max_shift=max_shift
            )
            prediction = ipvf.translate_frame(previous, dx, dy)
            residual = ipvf.xor_frames(prediction, current)
            compressed = ipvf.compress_lz4(residual, lz4_mode)
            payload_size = 2 + 4 + len(compressed)
            if len(compressed) < ipvf.FRAME_BYTES:
                motion_cost = ipvf.record_sectors(payload_size, audio_size)
                if motion_cost < selected_cost:
                    selected_cost = motion_cost
                    motion_records += 1
            reconstructed = bytes(a ^ b for a, b in zip(prediction, residual))
            if reconstructed != current:
                raise RuntimeError("translation residual failed roundtrip")
        baseline_sectors += baseline_cost
        adaptive_sectors += selected_cost
        previous = current
        frames += 1
    if frames == 0:
        raise RuntimeError("source produced no frames")
    saving = 1.0 - adaptive_sectors / baseline_sectors
    return {
        "source": str(source),
        "fps": f"{rate.numerator}/{rate.denominator}",
        "frames": frames,
        "baseline_bytes": baseline_sectors * 512,
        "motion_bytes": adaptive_sectors * 512,
        "saving_percent": saving * 100.0,
        "motion_records": motion_records,
    }


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("sources", nargs="+", type=Path)
    parser.add_argument("--ffmpeg", default="ffmpeg")
    parser.add_argument("--ffprobe", default="ffprobe")
    parser.add_argument("--max-shift", type=int, default=16)
    parser.add_argument("--keyint", type=int, default=120)
    parser.add_argument(
        "--lz4-mode", choices=("builtin", "best", "official-hc12"),
        default="best",
    )
    args = parser.parse_args()
    for source in args.sources:
        report = measure(
            source, args.ffmpeg, args.ffprobe, args.max_shift,
            args.keyint, args.lz4_mode,
        )
        print(
            f"{report['source']}: {report['frames']} frames @ {report['fps']}, "
            f"{report['baseline_bytes']:,} -> {report['motion_bytes']:,} bytes, "
            f"saving={report['saving_percent']:.2f}%, "
            f"motion-records={report['motion_records']}"
        )


if __name__ == "__main__":
    main()
