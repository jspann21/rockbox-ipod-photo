#!/usr/bin/env python3
"""Measure bounded whole-frame translation residuals without changing IPVF."""
from __future__ import annotations

import argparse
from fractions import Fraction
from pathlib import Path

import numpy as np

try:
    from . import encode as ipvf
except ImportError:
    import encode as ipvf


def block_prediction(previous: bytes, current: bytes, global_dx: int,
                     global_dy: int, block_size: int,
                     radius: int) -> tuple[bytes, bytes]:
    """Return a fixed-grid block prediction and signed vector table."""
    previous_array = np.frombuffer(previous, dtype=np.uint8).reshape(
        ipvf.H, ipvf.W, 2
    )
    current_array = np.frombuffer(current, dtype=np.uint8).reshape(
        ipvf.H, ipvf.W, 2
    )
    prediction = np.zeros_like(previous_array)
    vectors = bytearray()
    candidates = {(0, 0), (global_dx, global_dy)}
    for dy in range(global_dy - radius, global_dy + radius + 1):
        for dx in range(global_dx - radius, global_dx + radius + 1):
            if -127 <= dx <= 127 and -127 <= dy <= 127:
                candidates.add((dx, dy))

    for y0 in range(0, ipvf.H, block_size):
        y1 = min(ipvf.H, y0 + block_size)
        for x0 in range(0, ipvf.W, block_size):
            x1 = min(ipvf.W, x0 + block_size)
            target = current_array[y0:y1, x0:x1]
            best_score = None
            best_vector = (0, 0)
            best_prediction = None
            for dx, dy in candidates:
                candidate = np.zeros_like(target)
                target_x0 = max(x0, dx)
                target_x1 = min(x1, ipvf.W + dx)
                target_y0 = max(y0, dy)
                target_y1 = min(y1, ipvf.H + dy)
                if target_x0 < target_x1 and target_y0 < target_y1:
                    candidate[
                        target_y0 - y0:target_y1 - y0,
                        target_x0 - x0:target_x1 - x0,
                    ] = previous_array[
                        target_y0 - dy:target_y1 - dy,
                        target_x0 - dx:target_x1 - dx,
                    ]
                score = int(np.abs(
                    target.astype(np.int16) - candidate.astype(np.int16)
                ).sum())
                rank = (score, abs(dx) + abs(dy), dy, dx)
                if best_score is None or rank < best_score:
                    best_score = rank
                    best_vector = dx, dy
                    best_prediction = candidate
            assert best_prediction is not None
            prediction[y0:y1, x0:x1] = best_prediction
            vectors.extend((best_vector[0] & 0xff, best_vector[1] & 0xff))
    return prediction.tobytes(), bytes(vectors)


def measure(source: Path, ffmpeg: str, ffprobe: str, max_shift: int,
            keyint: int, lz4_mode: str, block_size: int,
            block_radius: int) -> dict[str, int | str | float]:
    rate = min(Fraction(30), ipvf.probe_source_fps(source, ffprobe))
    previous = None
    baseline_sectors = 0
    adaptive_sectors = 0
    block_sectors = 0
    motion_records = 0
    block_records = 0
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
        block_cost = baseline_cost
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
            block_cost = selected_cost
            block_predict, vectors = block_prediction(
                previous, current, dx, dy, block_size, block_radius
            )
            block_residual = ipvf.xor_frames(block_predict, current)
            block_compressed = ipvf.compress_lz4(block_residual, lz4_mode)
            block_payload_size = 4 + len(vectors) + len(block_compressed)
            candidate_cost = ipvf.record_sectors(
                block_payload_size, audio_size
            )
            if (len(block_compressed) < ipvf.FRAME_BYTES and
                    candidate_cost < block_cost):
                block_cost = candidate_cost
                block_records += 1
            if ipvf.xor_frames(block_predict, block_residual) != current:
                raise RuntimeError("block translation residual failed roundtrip")
        baseline_sectors += baseline_cost
        adaptive_sectors += selected_cost
        block_sectors += block_cost
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
        "block_bytes": block_sectors * 512,
        "block_saving_percent": (1.0 - block_sectors / baseline_sectors) * 100,
        "block_incremental_percent": (
            1.0 - block_sectors / adaptive_sectors
        ) * 100,
        "block_records": block_records,
    }


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("sources", nargs="+", type=Path)
    parser.add_argument("--ffmpeg", default="ffmpeg")
    parser.add_argument("--ffprobe", default="ffprobe")
    parser.add_argument("--max-shift", type=int, default=16)
    parser.add_argument("--keyint", type=int, default=120)
    parser.add_argument("--block-size", type=int, default=16)
    parser.add_argument("--block-radius", type=int, default=1)
    parser.add_argument(
        "--lz4-mode", choices=("builtin", "best", "official-hc12"),
        default="best",
    )
    args = parser.parse_args()
    for source in args.sources:
        report = measure(
            source, args.ffmpeg, args.ffprobe, args.max_shift,
            args.keyint, args.lz4_mode, args.block_size, args.block_radius,
        )
        print(
            f"{report['source']}: {report['frames']} frames @ {report['fps']}, "
            f"{report['baseline_bytes']:,} -> {report['motion_bytes']:,} bytes, "
            f"saving={report['saving_percent']:.2f}%, "
            f"motion-records={report['motion_records']}; "
            f"block={report['block_bytes']:,} bytes, "
            f"block-saving={report['block_saving_percent']:.2f}%, "
            f"incremental={report['block_incremental_percent']:.2f}%, "
            f"block-records={report['block_records']}"
        )


if __name__ == "__main__":
    main()
